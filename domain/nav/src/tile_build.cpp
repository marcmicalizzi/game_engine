// One tile of navigation mesh, out of a triangle soup, through Recast's voxel pipeline.
//
// The knobs and why they are set the way they are live in docs/subsystems/nav.md; the two that
// decide everything else are the cell size (the pipeline is quadratic in the tile's voxel count)
// and the agent radius (which is eroded out of the walkable area, so a path along this mesh
// already clears the walls by a radius and needs no correction downstream).

#include "nav_log.h"
#include "recast_backend.h"
#include "tile_format.h"

#include <core/base/assert.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/nav/tile.h>

#include <cmath>
#include <cstring>

ENGINE_LOG_CATEGORY_DEFINE(log_nav, "nav");

namespace engine::nav {
namespace {

using detail::TileHeader;
using detail::TileMesh;

static_assert(sizeof(Vec3) == 12, "nav hands Vec3 arrays to Recast as float triples");

// Recast's allocation helpers are C-style pairs. Exceptions are off, so a build's early returns
// need something that frees; these are the smallest thing that does.
template <class T, void (*Free)(T*)>
class RcOwned {
 public:
  explicit RcOwned(T* p) noexcept : p_(p) {}
  ~RcOwned() {
    if (p_ != nullptr) Free(p_);
  }
  RcOwned(const RcOwned&) = delete;
  RcOwned& operator=(const RcOwned&) = delete;
  T* get() const noexcept { return p_; }
  explicit operator bool() const noexcept { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

using OwnedHeightfield = RcOwned<rcHeightfield, rcFreeHeightField>;
using OwnedCompact = RcOwned<rcCompactHeightfield, rcFreeCompactHeightfield>;
using OwnedContours = RcOwned<rcContourSet, rcFreeContourSet>;
using OwnedPolyMesh = RcOwned<rcPolyMesh, rcFreePolyMesh>;
using OwnedDetail = RcOwned<rcPolyMeshDetail, rcFreePolyMeshDetail>;

// core/math has min and max for vectors, not for scalars; these are local rather than added to
// it, because a capability may not edit a core module (ADR-0027 decision 3).
constexpr f32 min_f(f32 a, f32 b) noexcept { return a < b ? a : b; }
constexpr f32 max_f(f32 a, f32 b) noexcept { return a > b ? a : b; }

// A voxel count from a world distance, never below one: a radius that rounds to zero would mean
// "the agent is a point" and put paths flush against the walls.
i32 voxels_up(f32 world, f32 cell) noexcept {
  const i32 n = static_cast<i32>(std::ceil(world / cell));
  return n < 0 ? 0 : n;
}

u32 find_root(Vector<u32>& parent, u32 i) noexcept {
  while (parent[i] != i) {
    parent[i] = parent[parent[i]];  // one-pass halving; the sets are tiny and this is not hot
    i = parent[i];
  }
  return i;
}

void unite(Vector<u32>& parent, u32 a, u32 b) noexcept {
  const u32 ra = find_root(parent, a);
  const u32 rb = find_root(parent, b);
  if (ra == rb) return;
  // Always attach the larger index to the smaller, so the representative of a set is its lowest
  // polygon index and the component numbering below is a function of the mesh alone.
  if (ra < rb)
    parent[rb] = ra;
  else
    parent[ra] = rb;
}

struct PortalEdge {
  f32 lo = 0.0f;
  f32 hi = 0.0f;
  f32 y_min = 0.0f;
  f32 y_max = 0.0f;
  f32 axis = 0.0f;
  u16 region = 0;
  u8 side = 0;
};

// Sorts by (region, side, lo) so that merging is one linear pass and the output order is a
// function of the mesh rather than of anything the sort happened to see first.
bool portal_less(const PortalEdge& a, const PortalEdge& b) noexcept {
  if (a.region != b.region) return a.region < b.region;
  if (a.side != b.side) return a.side < b.side;
  return a.lo < b.lo;
}

void sort_portals(Vector<PortalEdge>& edges) {
  // Insertion sort: a tile has tens of border edges, not thousands, and this keeps the module
  // free of a comparator that the standard library might order differently across versions.
  for (u32 i = 1; i < edges.size(); ++i) {
    PortalEdge key = edges[i];
    u32 j = i;
    while (j > 0 && portal_less(key, edges[j - 1])) {
      edges[j] = edges[j - 1];
      --j;
    }
    edges[j] = key;
  }
}

}  // namespace

const char* status_name(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "Ok";
    case Status::Partial: return "Partial";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::NotFound: return "NotFound";
    case Status::LimitReached: return "LimitReached";
    case Status::Unsupported: return "Unsupported";
    case Status::BackendError: return "BackendError";
  }
  return "Unknown";
}

const char* area_name(Area area) noexcept {
  switch (area) {
    case Area::Null: return "Null";
    case Area::Ground: return "Ground";
    case Area::Water: return "Water";
    case Area::Door: return "Door";
    case Area::Jump: return "Jump";
    case Area::Hazard: return "Hazard";
    case Area::Count: break;
  }
  return "Unknown";
}

f32 path_length(std::span<const Vec3> points) noexcept {
  f32 total = 0.0f;
  for (usize i = 1; i < points.size(); ++i)
    total += distance(points[i - 1], points[i]);
  return total;
}

// --- NavTileData --------------------------------------------------------------------------

bool NavTileData::from_bytes(std::span<const u8> bytes) {
  TileMesh mesh;
  if (!detail::decode_tile(bytes, mesh)) return false;

  bytes_.assign(bytes.begin(), bytes.end());
  regions_ = mesh.regions;
  portals_ = mesh.portals;
  coord_ = TileCoord{mesh.header.tile_x, mesh.header.tile_y};
  bounds_ = Aabb3{Vec3(mesh.header.bmin[0], mesh.header.bmin[1], mesh.header.bmin[2]),
                  Vec3(mesh.header.bmax[0], mesh.header.bmax[1], mesh.header.bmax[2])};
  grid_origin_ =
      Vec3(mesh.header.grid_origin[0], mesh.header.grid_origin[1], mesh.header.grid_origin[2]);
  grid_tile_size_ = mesh.header.grid_tile_size;
  agent_height_ = mesh.header.walkable_height;
  agent_radius_ = mesh.header.walkable_radius;
  agent_climb_ = mesh.header.walkable_climb;
  vert_count_ = mesh.header.vert_count;
  poly_count_ = mesh.header.poly_count;
  region_count_ = mesh.header.region_count;
  return true;
}

NavTileData NavTileData::clone() const {
  NavTileData copy;
  if (!bytes_.empty()) copy.from_bytes(bytes());
  return copy;
}

// --- parameters ---------------------------------------------------------------------------

bool validate(const NavBuildParams& params) noexcept {
  if (!(params.cell_size > 0.0f) || !(params.cell_height > 0.0f)) return false;
  if (!(params.tile_size > 0.0f)) return false;
  if (!(params.agent_radius >= 0.0f) || !(params.agent_height > 0.0f)) return false;
  if (!(params.agent_max_climb >= 0.0f)) return false;
  if (params.agent_max_slope_degrees <= 0.0f || params.agent_max_slope_degrees > 90.0f)
    return false;
  if (params.verts_per_poly < 3 || params.verts_per_poly > 6) return false;
  if (params.height_max <= params.height_min) return false;
  // The agent has to fit in the voxel grid, or every span is a ledge.
  if (params.agent_height < params.cell_height) return false;
  // A tile is at least as wide as the border the pipeline needs on each side, or there is no
  // inner region left to build.
  const f32 border = (params.agent_radius + 3.0f * params.cell_size);
  if (params.tile_size <= 2.0f * border) return false;
  // The seam rule. Recast's voxel grid for a tile is round(tile_size / cell_size) cells wide and
  // its border portals sit at voxel 0 and voxel N of that grid, so a tile size the cell size
  // does not divide covers less ground than the grid says it does — 64 m at 0.3 m cells reaches
  // 63.9 m — and Detour, which links two tiles by matching their portal coordinates to within a
  // centimetre, then links nothing. Everything builds, everything is added, and no path crosses
  // a tile border. Refusing the pair here is the only place this is cheap to notice.
  const f32 cells = params.tile_size / params.cell_size;
  const f32 rounded = static_cast<f32>(std::lround(cells));
  if (std::abs(cells - rounded) > 1.0e-3f) return false;
  return true;
}

Aabb3 tile_bounds(const NavBuildParams& params, TileCoord coord, bool expanded) noexcept {
  const f32 min_x = params.origin.x + static_cast<f32>(coord.x) * params.tile_size;
  const f32 min_z = params.origin.z + static_cast<f32>(coord.y) * params.tile_size;
  f32 pad = 0.0f;
  if (expanded) {
    const i32 border = voxels_up(params.agent_radius, params.cell_size) + 3;
    pad = static_cast<f32>(border) * params.cell_size;
  }
  return Aabb3{
      Vec3(min_x - pad, params.height_min, min_z - pad),
      Vec3(min_x + params.tile_size + pad, params.height_max, min_z + params.tile_size + pad)};
}

// --- the build ----------------------------------------------------------------------------

Status build_tile(const NavBuildParams& params, TileCoord coord, const TileGeometry& geometry,
                  NavTileData& out, TileBuildStats* stats) {
  if (stats != nullptr) *stats = TileBuildStats{};
  if (!validate(params)) return Status::InvalidArgument;
  if (geometry.indices.size() % 3 != 0) return Status::InvalidArgument;
  const usize triangle_count = geometry.indices.size() / 3;
  if (!geometry.areas.empty() && geometry.areas.size() != triangle_count)
    return Status::InvalidArgument;
  if (geometry.vertices.size() > 0x7FFFFFFFu) return Status::LimitReached;

  const f32 cs = params.cell_size;
  const f32 ch = params.cell_height;
  const i32 tile_voxels = static_cast<i32>(std::lround(params.tile_size / cs));
  if (tile_voxels < 8) return Status::InvalidArgument;

  const i32 border = voxels_up(params.agent_radius, cs) + 3;
  const f32 pad = static_cast<f32>(border) * cs;
  const Aabb3 tile = tile_bounds(params, coord, false);

  // --- pick the triangles that can touch this tile, and the y range they need --------------
  // The heightfield's vertical extent is fitted to the geometry rather than taken from
  // height_min/height_max: Recast clamps a triangle below the field onto its floor, which would
  // invent a walkable surface at the bottom of the column, and a column sized for the whole
  // world costs a span index per cell_height of it.
  const f32 select_min_x = tile.min.x - pad;
  const f32 select_max_x = tile.max.x + pad;
  const f32 select_min_z = tile.min.z - pad;
  const f32 select_max_z = tile.max.z + pad;

  Vector<i32> tris;
  Vector<u8> tri_areas;
  tris.reserve(static_cast<u32>(triangle_count * 3 / 4 + 3));
  tri_areas.reserve(static_cast<u32>(triangle_count / 4 + 1));
  f32 y_min = 3.4e38f;
  f32 y_max = -3.4e38f;
  const u32 vertex_count = static_cast<u32>(geometry.vertices.size());

  for (usize t = 0; t < triangle_count; ++t) {
    const u32 i0 = geometry.indices[t * 3 + 0];
    const u32 i1 = geometry.indices[t * 3 + 1];
    const u32 i2 = geometry.indices[t * 3 + 2];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
      return Status::InvalidArgument;
    const Vec3 a = geometry.vertices[i0];
    const Vec3 b = geometry.vertices[i1];
    const Vec3 c = geometry.vertices[i2];
    const Vec3 tri_min = engine::min(a, engine::min(b, c));
    const Vec3 tri_max = engine::max(a, engine::max(b, c));
    if (tri_max.x < select_min_x || tri_min.x > select_max_x) continue;
    if (tri_max.z < select_min_z || tri_min.z > select_max_z) continue;
    if (tri_max.y < params.height_min || tri_min.y > params.height_max) continue;
    y_min = tri_min.y < y_min ? tri_min.y : y_min;
    y_max = tri_max.y > y_max ? tri_max.y : y_max;

    tris.push_back(static_cast<i32>(i0));
    tris.push_back(static_cast<i32>(i1));
    tris.push_back(static_cast<i32>(i2));
    u8 area = static_cast<u8>(Area::Ground);
    if (!geometry.areas.empty()) area = geometry.areas[t];
    if (area >= k_max_areas) return Status::InvalidArgument;
    tri_areas.push_back(area);
  }

  if (stats != nullptr) {
    stats->triangles_in = static_cast<u32>(triangle_count);
    stats->triangles_rasterized = tri_areas.size();
  }

  out = NavTileData{};
  if (tri_areas.empty()) return Status::Ok;  // a tile with nothing in it is a legal empty tile

  y_min = max_f(y_min, params.height_min) - ch;
  y_max = min_f(y_max, params.height_max) + params.agent_height + ch;
  if (!((y_max - y_min) / ch < 60000.0f)) return Status::LimitReached;

  detail::SilentContext ctx;

  rcConfig cfg{};
  cfg.cs = cs;
  cfg.ch = ch;
  cfg.walkableSlopeAngle = params.agent_max_slope_degrees;
  cfg.walkableHeight = voxels_up(params.agent_height, ch);
  cfg.walkableClimb = static_cast<i32>(std::floor(params.agent_max_climb / ch));
  cfg.walkableRadius = voxels_up(params.agent_radius, cs);
  cfg.maxEdgeLen = static_cast<i32>(params.edge_max_len / cs);
  cfg.maxSimplificationError = params.edge_max_error;
  // Recast counts region sizes in voxel *areas*; the parameters are the side of a square in
  // metres, because a caller who has to know the cell size to ask for "drop islands under two
  // metres across" will get it wrong the first time the cell size changes.
  const f32 min_side = params.region_min_size / cs;
  const f32 merge_side = params.region_merge_size / cs;
  cfg.minRegionArea = static_cast<i32>(min_side * min_side);
  cfg.mergeRegionArea = static_cast<i32>(merge_side * merge_side);
  cfg.maxVertsPerPoly = static_cast<i32>(params.verts_per_poly);
  cfg.tileSize = tile_voxels;
  cfg.borderSize = border;
  cfg.width = cfg.tileSize + cfg.borderSize * 2;
  cfg.height = cfg.tileSize + cfg.borderSize * 2;
  cfg.detailSampleDist = params.detail_sample_dist < 0.9f ? 0.0f : cs * params.detail_sample_dist;
  cfg.detailSampleMaxError = ch * params.detail_sample_max_error;
  cfg.bmin[0] = tile.min.x - pad;
  cfg.bmin[1] = y_min;
  cfg.bmin[2] = tile.min.z - pad;
  cfg.bmax[0] = tile.max.x + pad;
  cfg.bmax[1] = y_max;
  cfg.bmax[2] = tile.max.z + pad;

  const float* verts = reinterpret_cast<const float*>(geometry.vertices.data());
  const i32 nverts = static_cast<i32>(vertex_count);
  const i32 ntris = static_cast<i32>(tri_areas.size());

  // Steep triangles lose their area id rather than gaining one, so a caller's area assignment
  // survives everywhere the slope allows it (rcMarkWalkableTriangles would overwrite all of it
  // with RC_WALKABLE_AREA).
  rcClearUnwalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nverts, tris.data(), ntris,
                             tri_areas.data());

  OwnedHeightfield hf(rcAllocHeightfield());
  if (!hf) return Status::BackendError;
  if (!rcCreateHeightfield(&ctx, *hf.get(), cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs,
                           cfg.ch))
    return Status::BackendError;
  if (!rcRasterizeTriangles(&ctx, verts, nverts, tris.data(), tri_areas.data(), ntris, *hf.get(),
                            cfg.walkableClimb))
    return Status::BackendError;

  if (params.filter_low_hanging_obstacles)
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf.get());
  if (params.filter_ledge_spans)
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf.get());
  if (params.filter_walkable_low_height_spans)
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf.get());

  if (stats != nullptr)
    stats->span_count = static_cast<u32>(rcGetHeightFieldSpanCount(&ctx, *hf.get()));

  OwnedCompact chf(rcAllocCompactHeightfield());
  if (!chf) return Status::BackendError;
  if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf.get(),
                                 *chf.get()))
    return Status::BackendError;
  if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf.get())) return Status::BackendError;

  if (params.monotone_regions) {
    if (!rcBuildRegionsMonotone(&ctx, *chf.get(), cfg.borderSize, cfg.minRegionArea,
                                cfg.mergeRegionArea))
      return Status::BackendError;
  } else {
    if (!rcBuildDistanceField(&ctx, *chf.get())) return Status::BackendError;
    if (!rcBuildRegions(&ctx, *chf.get(), cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
      return Status::BackendError;
  }

  OwnedContours cset(rcAllocContourSet());
  if (!cset) return Status::BackendError;
  if (!rcBuildContours(&ctx, *chf.get(), cfg.maxSimplificationError, cfg.maxEdgeLen, *cset.get()))
    return Status::BackendError;
  if (cset.get()->nconts == 0) return Status::Ok;  // nothing walkable: a legal empty tile

  OwnedPolyMesh pmesh(rcAllocPolyMesh());
  if (!pmesh) return Status::BackendError;
  if (!rcBuildPolyMesh(&ctx, *cset.get(), cfg.maxVertsPerPoly, *pmesh.get()))
    return Status::BackendError;
  if (pmesh.get()->npolys == 0) return Status::Ok;
  // Detour indexes vertices with 16 bits and reserves 0xffff; a tile that needs more has to be
  // split, which means a smaller tile size, and the caller has to know that rather than get a
  // silently truncated mesh.
  if (pmesh.get()->nverts >= 0xffff) {
    ENGINE_LOG_WARN(log_nav, "tile has more vertices than a Detour tile can index",
                    log::field("tile_x", static_cast<i64>(coord.x)),
                    log::field("tile_y", static_cast<i64>(coord.y)),
                    log::field("verts", static_cast<i64>(pmesh.get()->nverts)));
    return Status::LimitReached;
  }

  // The detail mesh refines the walkable surface's *height* inside each polygon: without one a
  // polygon is a plane and an agent on a ramp reads the wrong ground height. `detail_sample_dist`
  // below 0.9 asks for none, and then the stage is skipped outright rather than run with a
  // sample distance of zero — Detour builds implicit detail from the polygons themselves in that
  // case, and `rcBuildPolyMeshDetail` with a zero sample distance reads out of bounds and dies
  // (Recast 1.6.0; the `minExtent < sampleDist * 2` guard that protects slivers is dead when
  // sampleDist is 0). Skipping is cheaper and is what a caller asking for no detail mesh meant.
  const bool want_detail = cfg.detailSampleDist > 0.0f;
  OwnedDetail dmesh(want_detail ? rcAllocPolyMeshDetail() : nullptr);
  if (want_detail) {
    if (!dmesh) return Status::BackendError;
    if (!rcBuildPolyMeshDetail(&ctx, *pmesh.get(), *chf.get(), cfg.detailSampleDist,
                               cfg.detailSampleMaxError, *dmesh.get()))
      return Status::BackendError;
  }

  const rcPolyMesh& pm = *pmesh.get();
  const rcPolyMeshDetail* dm = dmesh.get();
  const u32 npolys = static_cast<u32>(pm.npolys);
  const u32 nvp = static_cast<u32>(pm.nvp);

  // --- flags --------------------------------------------------------------------------------
  TileMesh mesh;
  mesh.flags.resize(npolys);
  for (u32 i = 0; i < npolys; ++i) {
    const u8 area = pm.areas[i];
    mesh.flags[i] = area < k_area_count ? default_flags_for(static_cast<Area>(area)) : k_flag_walk;
  }

  // --- connected components -------------------------------------------------------------------
  // Recast's own region ids are not connectivity: `rcBuildRegions` splits a walkable area into
  // as many regions as the watershed found, and merges small ones into whichever neighbour was
  // convenient. The region graph needs "can an agent walk from this polygon to that one without
  // leaving the tile", which is the connected components of the polygon adjacency and nothing
  // else.
  Vector<u32> parent(npolys);
  for (u32 i = 0; i < npolys; ++i)
    parent[i] = i;
  for (u32 i = 0; i < npolys; ++i) {
    const u16* p = &pm.polys[i * 2 * nvp];
    for (u32 j = 0; j < nvp; ++j) {
      if (p[j] == RC_MESH_NULL_IDX) break;
      const u16 nei = p[nvp + j];
      if (nei == RC_MESH_NULL_IDX || (nei & 0x8000) != 0) continue;
      unite(parent, i, nei);
    }
  }

  mesh.region.resize(npolys);
  Vector<u32> component_of_root(npolys);
  for (u32 i = 0; i < npolys; ++i)
    component_of_root[i] = 0xFFFFFFFFu;
  u32 component_count = 0;
  for (u32 i = 0; i < npolys; ++i) {
    const u32 root = find_root(parent, i);
    if (component_of_root[root] == 0xFFFFFFFFu) {
      component_of_root[root] = component_count;
      ++component_count;
    }
    mesh.region[i] = static_cast<u16>(component_of_root[root]);
  }

  // --- per-region summary ---------------------------------------------------------------------
  mesh.regions.resize(component_count);
  for (u32 i = 0; i < component_count; ++i)
    mesh.regions[i].bounds = Aabb3::empty();
  Vector<Vec3> weighted(component_count);
  for (u32 i = 0; i < component_count; ++i)
    weighted[i] = Vec3(0.0f, 0.0f, 0.0f);

  const f32 bmin_x = pm.bmin[0];
  const f32 bmin_y = pm.bmin[1];
  const f32 bmin_z = pm.bmin[2];
  auto world_vertex = [&](u16 index) noexcept {
    const u16* v = &pm.verts[static_cast<usize>(index) * 3];
    return Vec3(bmin_x + static_cast<f32>(v[0]) * cs, bmin_y + static_cast<f32>(v[1]) * ch,
                bmin_z + static_cast<f32>(v[2]) * cs);
  };

  Vector<PortalEdge> portal_edges;
  for (u32 i = 0; i < npolys; ++i) {
    const u16* p = &pm.polys[i * 2 * nvp];
    u32 vertex_n = 0;
    while (vertex_n < nvp && p[vertex_n] != RC_MESH_NULL_IDX)
      ++vertex_n;
    if (vertex_n < 3) continue;

    const u32 component = mesh.region[i];
    TileRegionInfo& info = mesh.regions[component];
    info.poly_count += 1;

    // Shoelace over the horizontal projection, and a centroid weighted by it, so a long thin
    // corridor's node sits in the corridor rather than at the average of its corners.
    f32 twice_area = 0.0f;
    Vec3 sum(0.0f, 0.0f, 0.0f);
    for (u32 j = 0; j < vertex_n; ++j) {
      const Vec3 a = world_vertex(p[j]);
      const Vec3 b = world_vertex(p[(j + 1) % vertex_n]);
      twice_area += a.x * b.z - b.x * a.z;
      sum = sum + a;
      info.bounds.expand(a);
    }
    const f32 poly_area = std::abs(twice_area) * 0.5f;
    const Vec3 centroid = sum / static_cast<f32>(vertex_n);
    info.area += poly_area;
    weighted[component] = weighted[component] + centroid * poly_area;

    for (u32 j = 0; j < vertex_n; ++j) {
      const u16 nei = p[nvp + j];
      if ((nei & 0x8000) == 0) continue;
      const u16 dir = static_cast<u16>(nei & 0xf);
      if (dir > 3) continue;  // 0xf is a solid border, not a portal to the next tile
      const Vec3 a = world_vertex(p[j]);
      const Vec3 b = world_vertex(p[(j + 1) % vertex_n]);
      PortalEdge edge;
      edge.region = static_cast<u16>(component);
      edge.side = static_cast<u8>(dir);
      edge.y_min = min_f(a.y, b.y);
      edge.y_max = max_f(a.y, b.y);
      if (dir == 0 || dir == 2) {
        edge.axis = (a.x + b.x) * 0.5f;
        edge.lo = min_f(a.z, b.z);
        edge.hi = max_f(a.z, b.z);
      } else {
        edge.axis = (a.z + b.z) * 0.5f;
        edge.lo = min_f(a.x, b.x);
        edge.hi = max_f(a.x, b.x);
      }
      portal_edges.push_back(edge);
    }
  }

  for (u32 i = 0; i < component_count; ++i) {
    TileRegionInfo& info = mesh.regions[i];
    info.centroid = info.area > 0.0f ? weighted[i] / info.area : info.bounds.center();
  }

  // --- merge the border edges into stretches ---------------------------------------------------
  // One edge per polygon side would give the region graph a few hundred identical edges between
  // the same two nodes. Merging runs of edges that touch, share a region and a side, and overlap
  // in y keeps the graph's edge count proportional to the number of doorways.
  sort_portals(portal_edges);
  const f32 join_eps = cs * 1.5f;
  const f32 climb = params.agent_max_climb + ch;
  for (u32 i = 0; i < portal_edges.size(); ++i) {
    const PortalEdge& e = portal_edges[i];
    bool merged = false;
    if (!mesh.portals.empty()) {
      TilePortal& last = mesh.portals.back();
      if (last.region == e.region && last.side == e.side && e.lo <= last.hi + join_eps &&
          e.y_min <= last.y_max + climb && e.y_max + climb >= last.y_min) {
        last.hi = max_f(last.hi, e.hi);
        last.y_min = min_f(last.y_min, e.y_min);
        last.y_max = max_f(last.y_max, e.y_max);
        merged = true;
      }
    }
    if (!merged) {
      TilePortal portal;
      portal.lo = e.lo;
      portal.hi = e.hi;
      portal.y_min = e.y_min;
      portal.y_max = e.y_max;
      portal.axis = e.axis;
      portal.region = e.region;
      portal.side = e.side;
      portal.pad = 0;
      mesh.portals.push_back(portal);
    }
  }

  // --- pack -------------------------------------------------------------------------------------
  TileHeader& header = mesh.header;
  header.tile_x = coord.x;
  header.tile_y = coord.y;
  header.grid_origin[0] = params.origin.x;
  header.grid_origin[1] = params.origin.y;
  header.grid_origin[2] = params.origin.z;
  header.grid_tile_size = params.tile_size;
  for (u32 i = 0; i < 3; ++i) {
    header.bmin[i] = pm.bmin[i];
    header.bmax[i] = pm.bmax[i];
  }
  header.cs = cs;
  header.ch = ch;
  header.walkable_height = params.agent_height;
  header.walkable_radius = params.agent_radius;
  header.walkable_climb = params.agent_max_climb;
  header.nvp = nvp;
  header.vert_count = static_cast<u32>(pm.nverts);
  header.poly_count = npolys;
  header.detail_mesh_count = dm != nullptr ? static_cast<u32>(dm->nmeshes) : 0;
  header.detail_vert_count = dm != nullptr ? static_cast<u32>(dm->nverts) : 0;
  header.detail_tri_count = dm != nullptr ? static_cast<u32>(dm->ntris) : 0;
  header.region_count = component_count;
  header.portal_count = mesh.portals.size();

  mesh.verts.assign(pm.verts, pm.verts + static_cast<usize>(pm.nverts) * 3);
  mesh.polys.assign(pm.polys, pm.polys + static_cast<usize>(npolys) * 2 * nvp);
  mesh.areas.assign(pm.areas, pm.areas + npolys);
  if (dm != nullptr) {
    if (dm->nmeshes > 0)
      mesh.detail_meshes.assign(dm->meshes, dm->meshes + static_cast<usize>(dm->nmeshes) * 4);
    if (dm->nverts > 0)
      mesh.detail_verts.assign(dm->verts, dm->verts + static_cast<usize>(dm->nverts) * 3);
    if (dm->ntris > 0)
      mesh.detail_tris.assign(dm->tris, dm->tris + static_cast<usize>(dm->ntris) * 4);
  }

  Vector<u8> bytes;
  detail::encode_tile(mesh, bytes);
  if (!out.from_bytes(std::span<const u8>(bytes.data(), bytes.size()))) return Status::BackendError;

  if (stats != nullptr) {
    stats->vert_count = header.vert_count;
    stats->poly_count = header.poly_count;
    stats->region_count = header.region_count;
  }
  return Status::Ok;
}

}  // namespace engine::nav
