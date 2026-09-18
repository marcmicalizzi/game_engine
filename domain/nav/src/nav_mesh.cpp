// The navigation mesh: Detour's tile mesh and one query object, behind engine types.
//
// The one structural decision worth knowing before reading: a tile is stored as the *polygon
// mesh* the builder produced, and Detour's tile data is baked from it on every change. That is
// what makes "add an off-mesh link at run time" a local operation — the link has to be inside
// the tile's Detour data, and re-baking one tile from its polygon mesh costs a bounding-volume
// tree over a few hundred polygons instead of a voxelization.

#include "nav_log.h"
#include "recast_backend.h"
#include "tile_format.h"

#include <core/base/assert.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <domain/nav/nav_mesh.h>

#include <cmath>
#include <cstring>

namespace engine::nav {
namespace {

constexpr f32 min_f(f32 a, f32 b) noexcept { return a < b ? a : b; }

void to_array(Vec3 v, float out[3]) noexcept {
  out[0] = v.x;
  out[1] = v.y;
  out[2] = v.z;
}

Vec3 from_array(const float v[3]) noexcept { return Vec3(v[0], v[1], v[2]); }

void fill_filter(const PathFilter& filter, dtQueryFilter& out) noexcept {
  out.setIncludeFlags(filter.include_flags);
  out.setExcludeFlags(filter.exclude_flags);
  for (u32 i = 0; i < k_max_areas; ++i) {
    // A cost below 1 makes Detour's Euclidean heuristic inadmissible and the search returns
    // paths that are not merely suboptimal but visibly wrong, so the filter clamps rather than
    // trusting the caller. Costs above 1 are the point of the table and pass through.
    f32 cost = 1.0f;
    if (i < k_area_count && filter.area_cost[i] > 1.0f) cost = filter.area_cost[i];
    out.setAreaCost(static_cast<int>(i), cost);
  }
}

// How many bits Detour will spend on tiles and polygons for these caps. Detour needs at least
// DT_SALT_BITS (10) left over in a 32-bit polygon reference.
u32 bits_for(u32 value) noexcept {
  u32 bits = 0;
  while ((1u << bits) < value)
    ++bits;
  return bits;
}

struct TileEntry {
  Vector<u8> bytes;
  dtTileRef ref = 0;
};

}  // namespace

struct NavMesh::Impl {
  NavMeshOptions options;
  dtNavMesh* mesh = nullptr;
  dtNavMeshQuery* query = nullptr;
  HashMap<u64, TileEntry> tiles;
  // Links are keyed by the tile that owns them — the tile containing the link's *start* point —
  // whether or not that tile is resident, so a link added before its tile arrives is baked in
  // when the tile does.
  HashMap<u64, Vector<OffMeshLinkId>> tile_links;
  SlotMap<OffMeshLink> links;
  mutable Vector<dtPolyRef> path_scratch;
  u64 revision = 0;

  Status bake(u64 key, TileEntry& entry);
  void unbake(TileEntry& entry) noexcept;
};

void NavMesh::Impl::unbake(TileEntry& entry) noexcept {
  if (entry.ref != 0) {
    mesh->removeTile(entry.ref, nullptr, nullptr);
    entry.ref = 0;
  }
}

Status NavMesh::Impl::bake(u64 key, TileEntry& entry) {
  unbake(entry);
  detail::TileMesh mesh_data;
  if (!detail::decode_tile(std::span<const u8>(entry.bytes.data(), entry.bytes.size()), mesh_data))
    return Status::InvalidArgument;
  if (mesh_data.header.poly_count == 0) return Status::Ok;

  // Gather this tile's links into the flat arrays Detour wants.
  Vector<f32> link_verts;
  Vector<f32> link_radius;
  Vector<u16> link_flags;
  Vector<u8> link_areas;
  Vector<u8> link_dir;
  Vector<u32> link_ids;
  if (const Vector<OffMeshLinkId>* owned = tile_links.find_value(key); owned != nullptr) {
    for (const OffMeshLinkId id : *owned) {
      const OffMeshLink* link = links.get(id.handle);
      if (link == nullptr) continue;
      link_verts.push_back(link->start.x);
      link_verts.push_back(link->start.y);
      link_verts.push_back(link->start.z);
      link_verts.push_back(link->end.x);
      link_verts.push_back(link->end.y);
      link_verts.push_back(link->end.z);
      link_radius.push_back(link->radius);
      link_flags.push_back(link->flags);
      link_areas.push_back(static_cast<u8>(link->area));
      link_dir.push_back(link->bidirectional ? static_cast<u8>(DT_OFFMESH_CON_BIDIR) : u8{0});
      link_ids.push_back(static_cast<u32>(id.handle.to_u64() & 0xFFFFFFFFu));
    }
  }

  dtNavMeshCreateParams params{};
  params.verts = mesh_data.verts.data();
  params.vertCount = static_cast<int>(mesh_data.header.vert_count);
  params.polys = mesh_data.polys.data();
  params.polyFlags = mesh_data.flags.data();
  params.polyAreas = mesh_data.areas.data();
  params.polyCount = static_cast<int>(mesh_data.header.poly_count);
  params.nvp = static_cast<int>(mesh_data.header.nvp);
  if (mesh_data.header.detail_mesh_count != 0) {
    params.detailMeshes = mesh_data.detail_meshes.data();
    params.detailVerts = mesh_data.detail_verts.data();
    params.detailVertsCount = static_cast<int>(mesh_data.header.detail_vert_count);
    params.detailTris = mesh_data.detail_tris.data();
    params.detailTriCount = static_cast<int>(mesh_data.header.detail_tri_count);
  }
  if (!link_radius.empty()) {
    params.offMeshConVerts = link_verts.data();
    params.offMeshConRad = link_radius.data();
    params.offMeshConFlags = link_flags.data();
    params.offMeshConAreas = link_areas.data();
    params.offMeshConDir = link_dir.data();
    params.offMeshConUserID = link_ids.data();
    params.offMeshConCount = static_cast<int>(link_radius.size());
  }
  params.walkableHeight = mesh_data.header.walkable_height;
  params.walkableRadius = mesh_data.header.walkable_radius;
  params.walkableClimb = mesh_data.header.walkable_climb;
  params.tileX = mesh_data.header.tile_x;
  params.tileY = mesh_data.header.tile_y;
  params.tileLayer = 0;
  for (u32 i = 0; i < 3; ++i) {
    params.bmin[i] = mesh_data.header.bmin[i];
    params.bmax[i] = mesh_data.header.bmax[i];
  }
  params.cs = mesh_data.header.cs;
  params.ch = mesh_data.header.ch;
  params.buildBvTree = true;

  unsigned char* data = nullptr;
  int data_size = 0;
  if (!dtCreateNavMeshData(&params, &data, &data_size)) return Status::BackendError;

  dtTileRef ref = 0;
  // DT_TILE_FREE_DATA: Detour owns the buffer it was handed and frees it when the tile is
  // removed, which is what keeps every path out of this function from needing a free of its own.
  const dtStatus status = mesh->addTile(data, data_size, DT_TILE_FREE_DATA, 0, &ref);
  if (dtStatusFailed(status)) {
    dtFree(data);
    ENGINE_LOG_WARN(log_nav, "the backend refused a tile",
                    log::field("tile_x", static_cast<i64>(params.tileX)),
                    log::field("tile_y", static_cast<i64>(params.tileY)),
                    log::field("polys", static_cast<i64>(params.polyCount)));
    return dtStatusDetail(status, DT_OUT_OF_MEMORY) ? Status::LimitReached : Status::BackendError;
  }
  entry.ref = ref;
  return Status::Ok;
}

// --- lifetime ---------------------------------------------------------------------------------

NavMesh::NavMesh() noexcept = default;

NavMesh::~NavMesh() { shutdown(); }

Status NavMesh::init(const NavMeshOptions& options) {
  if (impl_ != nullptr) return Status::InvalidArgument;
  if (!(options.tile_size > 0.0f)) return Status::InvalidArgument;
  if (options.max_tiles == 0 || options.max_polys_per_tile == 0) return Status::InvalidArgument;
  if (options.max_path_polys < 4 || options.max_search_nodes < 16) return Status::InvalidArgument;
  if (options.max_search_nodes > 65535) return Status::InvalidArgument;
  // A 32-bit polygon reference is salt | tile | polygon and Detour keeps 10 bits of salt. Saying
  // so here, with the numbers, beats dtNavMesh::init's silent DT_FAILURE.
  if (bits_for(options.max_tiles) + bits_for(options.max_polys_per_tile) > 22) {
    ENGINE_LOG_ERROR(log_nav, "max_tiles and max_polys_per_tile do not fit a 32-bit poly ref",
                     log::field("max_tiles", static_cast<i64>(options.max_tiles)),
                     log::field("max_polys_per_tile", static_cast<i64>(options.max_polys_per_tile)),
                     log::field("bits", static_cast<i64>(bits_for(options.max_tiles) +
                                                         bits_for(options.max_polys_per_tile))));
    return Status::LimitReached;
  }

  auto* impl = new Impl();
  impl->options = options;
  impl->mesh = dtAllocNavMesh();
  impl->query = dtAllocNavMeshQuery();
  if (impl->mesh == nullptr || impl->query == nullptr) {
    if (impl->mesh != nullptr) dtFreeNavMesh(impl->mesh);
    if (impl->query != nullptr) dtFreeNavMeshQuery(impl->query);
    delete impl;
    return Status::BackendError;
  }

  dtNavMeshParams nav_params{};
  nav_params.orig[0] = options.origin.x;
  nav_params.orig[1] = options.origin.y;
  nav_params.orig[2] = options.origin.z;
  nav_params.tileWidth = options.tile_size;
  nav_params.tileHeight = options.tile_size;
  nav_params.maxTiles = static_cast<int>(options.max_tiles);
  nav_params.maxPolys = static_cast<int>(options.max_polys_per_tile);
  if (dtStatusFailed(impl->mesh->init(&nav_params))) {
    dtFreeNavMesh(impl->mesh);
    dtFreeNavMeshQuery(impl->query);
    delete impl;
    return Status::BackendError;
  }
  if (dtStatusFailed(impl->query->init(impl->mesh, static_cast<int>(options.max_search_nodes)))) {
    dtFreeNavMesh(impl->mesh);
    dtFreeNavMeshQuery(impl->query);
    delete impl;
    return Status::BackendError;
  }
  impl->path_scratch.resize(options.max_path_polys);
  impl->links.reserve(options.max_off_mesh_links);
  impl_ = impl;
  return Status::Ok;
}

void NavMesh::shutdown() noexcept {
  if (impl_ == nullptr) return;
  // dtNavMesh frees every tile it owns (DT_TILE_FREE_DATA) in its destructor.
  if (impl_->query != nullptr) dtFreeNavMeshQuery(impl_->query);
  if (impl_->mesh != nullptr) dtFreeNavMesh(impl_->mesh);
  delete impl_;
  impl_ = nullptr;
}

const NavMeshOptions& NavMesh::options() const noexcept {
  static const NavMeshOptions k_empty;
  return impl_ != nullptr ? impl_->options : k_empty;
}

void* NavMesh::backend() const noexcept { return impl_ != nullptr ? impl_->mesh : nullptr; }

u64 NavMesh::revision() const noexcept { return impl_ != nullptr ? impl_->revision : 0; }

// --- tiles ------------------------------------------------------------------------------------

Status NavMesh::add_tile(const NavTileData& tile) {
  if (impl_ == nullptr) return Status::NotFound;
  if (tile.empty()) return Status::InvalidArgument;
  // The grid check that makes "tiles are aligned to the world grid" a fact rather than a habit.
  // A mesh whose tiles disagree about their corners produces paths that stop at a tile border
  // with no error anywhere, which is the kind of bug that costs a day.
  const Vec3 grid = tile.grid_origin();
  const NavMeshOptions& opts = impl_->options;
  if (std::abs(grid.x - opts.origin.x) > 1.0e-3f || std::abs(grid.z - opts.origin.z) > 1.0e-3f ||
      std::abs(tile.grid_tile_size() - opts.tile_size) > 1.0e-3f) {
    ENGINE_LOG_ERROR(log_nav, "a tile was built on a different grid than this mesh uses",
                     log::field("tile_x", static_cast<i64>(tile.coord().x)),
                     log::field("tile_y", static_cast<i64>(tile.coord().y)));
    return Status::InvalidArgument;
  }

  const u64 key = tile_key(tile.coord());
  if (!tile.walkable()) {
    remove_tile(tile.coord());
    return Status::Ok;
  }

  TileEntry* entry = impl_->tiles.find_value(key);
  if (entry == nullptr) {
    if (impl_->tiles.size() >= opts.max_tiles) return Status::LimitReached;
    entry = &impl_->tiles.try_emplace(key).first->second;
  }
  const std::span<const u8> bytes = tile.bytes();
  entry->bytes.assign(bytes.begin(), bytes.end());
  const Status status = impl_->bake(key, *entry);
  if (status != Status::Ok) {
    impl_->tiles.erase(key);
    return status;
  }
  ++impl_->revision;
  return Status::Ok;
}

bool NavMesh::remove_tile(TileCoord coord) {
  if (impl_ == nullptr) return false;
  const u64 key = tile_key(coord);
  TileEntry* entry = impl_->tiles.find_value(key);
  if (entry == nullptr) return false;
  impl_->unbake(*entry);
  impl_->tiles.erase(key);
  ++impl_->revision;
  return true;
}

bool NavMesh::contains_tile(TileCoord coord) const noexcept {
  return impl_ != nullptr && impl_->tiles.contains(tile_key(coord));
}

u32 NavMesh::tile_count() const noexcept { return impl_ != nullptr ? impl_->tiles.size() : 0; }

u32 NavMesh::poly_count() const noexcept {
  if (impl_ == nullptr) return 0;
  u32 total = 0;
  for (const TileEntry& entry : impl_->tiles.values()) {
    detail::TileHeader header;
    if (detail::decode_tile_header(std::span<const u8>(entry.bytes.data(), entry.bytes.size()),
                                   header))
      total += header.poly_count;
  }
  return total;
}

std::span<const u8> NavMesh::tile_bytes(TileCoord coord) const noexcept {
  if (impl_ == nullptr) return {};
  const TileEntry* entry = impl_->tiles.find_value(tile_key(coord));
  if (entry == nullptr) return {};
  return {entry->bytes.data(), entry->bytes.size()};
}

TileCoord NavMesh::tile_containing(Vec3 point) const noexcept {
  if (impl_ == nullptr) return TileCoord{};
  return nav::tile_containing(point, impl_->options.origin, impl_->options.tile_size);
}

Aabb3 NavMesh::bounds() const noexcept {
  Aabb3 box = Aabb3::empty();
  if (impl_ == nullptr) return box;
  for (const TileEntry& entry : impl_->tiles.values()) {
    detail::TileHeader header;
    if (!detail::decode_tile_header(std::span<const u8>(entry.bytes.data(), entry.bytes.size()),
                                    header))
      continue;
    box.expand(Vec3(header.bmin[0], header.bmin[1], header.bmin[2]));
    box.expand(Vec3(header.bmax[0], header.bmax[1], header.bmax[2]));
  }
  return box;
}

// --- off-mesh links ---------------------------------------------------------------------------

Status NavMesh::add_off_mesh_link(const OffMeshLink& link, OffMeshLinkId& out) {
  out = OffMeshLinkId{};
  if (impl_ == nullptr) return Status::NotFound;
  if (!(link.radius > 0.0f)) return Status::InvalidArgument;
  if (impl_->links.size() >= impl_->options.max_off_mesh_links) return Status::LimitReached;

  const OffMeshLinkId id{impl_->links.insert(link)};
  const u64 key = tile_key(tile_containing(link.start));
  impl_->tile_links[key].push_back(id);

  if (TileEntry* entry = impl_->tiles.find_value(key); entry != nullptr) {
    const Status status = impl_->bake(key, *entry);
    if (status != Status::Ok) {
      // Put the tile back the way it was rather than leaving a hole where a tile used to be.
      Vector<OffMeshLinkId>& owned = impl_->tile_links[key];
      owned.pop_back();
      impl_->links.erase(id.handle);
      impl_->bake(key, *entry);
      return status;
    }
  }
  out = id;
  ++impl_->revision;
  return Status::Ok;
}

bool NavMesh::remove_off_mesh_link(OffMeshLinkId id) {
  if (impl_ == nullptr) return false;
  const OffMeshLink* link = impl_->links.get(id.handle);
  if (link == nullptr) return false;
  const u64 key = tile_key(tile_containing(link->start));
  if (Vector<OffMeshLinkId>* owned = impl_->tile_links.find_value(key); owned != nullptr) {
    for (u32 i = 0; i < owned->size(); ++i) {
      if ((*owned)[i] == id) {
        owned->erase(owned->begin() + i);
        break;
      }
    }
  }
  impl_->links.erase(id.handle);
  if (TileEntry* entry = impl_->tiles.find_value(key); entry != nullptr) impl_->bake(key, *entry);
  ++impl_->revision;
  return true;
}

bool NavMesh::off_mesh_link(OffMeshLinkId id, OffMeshLink& out) const {
  if (impl_ == nullptr) return false;
  const OffMeshLink* link = impl_->links.get(id.handle);
  if (link == nullptr) return false;
  out = *link;
  return true;
}

u32 NavMesh::off_mesh_link_count() const noexcept {
  return impl_ != nullptr ? impl_->links.size() : 0;
}

// --- queries ----------------------------------------------------------------------------------

bool NavMesh::find_nearest(Vec3 point, const PathFilter& filter, NavPoint& out) const {
  if (impl_ == nullptr) return false;
  return find_nearest(point, impl_->options.search_half_extents, filter, out);
}

bool NavMesh::find_nearest(Vec3 point, Vec3 half_extents, const PathFilter& filter,
                           NavPoint& out) const {
  out = NavPoint{};
  if (impl_ == nullptr) return false;
  dtQueryFilter query_filter;
  fill_filter(filter, query_filter);
  float centre[3];
  float extents[3];
  float nearest[3] = {0.0f, 0.0f, 0.0f};
  to_array(point, centre);
  to_array(half_extents, extents);
  dtPolyRef ref = 0;
  if (dtStatusFailed(impl_->query->findNearestPoly(centre, extents, &query_filter, &ref, nearest)))
    return false;
  if (ref == 0) return false;
  out.poly = ref;
  out.position = from_array(nearest);
  return true;
}

Status NavMesh::find_path(Vec3 from, Vec3 to, const PathFilter& filter, std::span<Vec3> corridor,
                          PathResult& out) const {
  out = PathResult{};
  if (impl_ == nullptr) return Status::NotFound;
  if (corridor.size() < 2) return Status::InvalidArgument;

  dtQueryFilter query_filter;
  fill_filter(filter, query_filter);
  float extents[3];
  to_array(impl_->options.search_half_extents, extents);

  float from_pos[3];
  float to_pos[3];
  float start_pt[3] = {0.0f, 0.0f, 0.0f};
  float end_pt[3] = {0.0f, 0.0f, 0.0f};
  to_array(from, from_pos);
  to_array(to, to_pos);
  dtPolyRef start_ref = 0;
  dtPolyRef end_ref = 0;
  if (dtStatusFailed(
          impl_->query->findNearestPoly(from_pos, extents, &query_filter, &start_ref, start_pt)) ||
      start_ref == 0)
    return Status::NotFound;
  if (dtStatusFailed(
          impl_->query->findNearestPoly(to_pos, extents, &query_filter, &end_ref, end_pt)) ||
      end_ref == 0)
    return Status::NotFound;

  const int max_polys = static_cast<int>(impl_->path_scratch.size());
  int poly_count = 0;
  const dtStatus find_status =
      impl_->query->findPath(start_ref, end_ref, start_pt, end_pt, &query_filter,
                             impl_->path_scratch.data(), &poly_count, max_polys);
  if (dtStatusFailed(find_status) || poly_count == 0) return Status::BackendError;

  bool partial = dtStatusDetail(find_status, DT_PARTIAL_RESULT) ||
                 dtStatusDetail(find_status, DT_BUFFER_TOO_SMALL);
  float target[3] = {0.0f, 0.0f, 0.0f};
  if (impl_->path_scratch[static_cast<u32>(poly_count - 1)] != end_ref) {
    // The search stopped short. The closest point on the last polygon is the honest answer for
    // an agent that still has to start walking.
    partial = true;
    if (dtStatusFailed(impl_->query->closestPointOnPoly(
            impl_->path_scratch[static_cast<u32>(poly_count - 1)], end_pt, target, nullptr)))
      return Status::BackendError;
  } else {
    target[0] = end_pt[0];
    target[1] = end_pt[1];
    target[2] = end_pt[2];
  }

  int straight_count = 0;
  const dtStatus pull_status =
      impl_->query->findStraightPath(start_pt, target, impl_->path_scratch.data(), poly_count,
                                     reinterpret_cast<float*>(corridor.data()), nullptr, nullptr,
                                     &straight_count, static_cast<int>(corridor.size()), 0);
  if (dtStatusFailed(pull_status)) return Status::BackendError;

  out.count = static_cast<u32>(straight_count);
  out.length = path_length(corridor.subspan(0, out.count));
  out.partial = partial;
  if (dtStatusDetail(pull_status, DT_BUFFER_TOO_SMALL)) return Status::LimitReached;
  return partial ? Status::Partial : Status::Ok;
}

bool NavMesh::raycast(Vec3 from, Vec3 to, const PathFilter& filter, RaycastHit& out) const {
  out = RaycastHit{};
  if (impl_ == nullptr) return false;

  dtQueryFilter query_filter;
  fill_filter(filter, query_filter);
  float extents[3];
  to_array(impl_->options.search_half_extents, extents);
  float from_pos[3];
  float to_pos[3];
  float start_pt[3] = {0.0f, 0.0f, 0.0f};
  to_array(from, from_pos);
  to_array(to, to_pos);
  dtPolyRef start_ref = 0;
  if (dtStatusFailed(
          impl_->query->findNearestPoly(from_pos, extents, &query_filter, &start_ref, start_pt)) ||
      start_ref == 0)
    return false;

  float t = 0.0f;
  float normal[3] = {0.0f, 0.0f, 0.0f};
  int poly_count = 0;
  const dtStatus status = impl_->query->raycast(start_ref, start_pt, to_pos, &query_filter, &t,
                                                normal, impl_->path_scratch.data(), &poly_count,
                                                static_cast<int>(impl_->path_scratch.size()));
  if (dtStatusFailed(status)) return false;

  const Vec3 origin = from_array(start_pt);
  if (t > 1.0f) {
    // Detour reports FLT_MAX when the segment reached its end without crossing a wall.
    out.hit = false;
    out.fraction = 1.0f;
    out.position = to;
    return true;
  }
  out.hit = true;
  out.fraction = min_f(t, 1.0f);
  out.position = origin + (to - origin) * out.fraction;
  out.normal = from_array(normal);
  return true;
}

}  // namespace engine::nav
