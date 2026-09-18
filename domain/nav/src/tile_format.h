#pragma once

// The bytes of a built tile: a polygon mesh plus the region summary, in one flat buffer.
//
// It is the *polygon* mesh and not Detour's tile format on purpose. An off-mesh link has to be
// baked into a tile's Detour data (`dtNavMeshCreateParams::offMeshCon*`), and links are added and
// removed at run time, so something has to be able to produce Detour data for a tile more than
// once. Keeping the polygon mesh as the stored form gives every tile exactly one source of
// truth: `NavMesh::add_tile` bakes from it, adding a link re-bakes from it, and the bytes a
// determinism test compares are the bytes the builder produced rather than a downstream encoding
// that also contains a bounding-volume tree built from them.
//
// Layout: `TileHeader`, then the sections below in order, each padded to a four-byte boundary.
// Nothing here is a file format — no endianness, no forward compatibility beyond the version
// check — so a buffer is a cache entry, never an asset (docs/subsystems/nav.md).

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/nav/tile.h>
#include <domain/nav/types.h>

#include <span>

namespace engine::nav::detail {

inline constexpr u32 k_tile_magic = 0x3154564Eu;  // "NVT1"
inline constexpr u32 k_tile_version = 1;

struct TileHeader {
  u32 magic = k_tile_magic;
  u32 version = k_tile_version;
  i32 tile_x = 0;
  i32 tile_y = 0;
  f32 grid_origin[3] = {0.0f, 0.0f, 0.0f};
  f32 grid_tile_size = 0.0f;
  f32 bmin[3] = {0.0f, 0.0f, 0.0f};
  f32 bmax[3] = {0.0f, 0.0f, 0.0f};
  f32 cs = 0.0f;
  f32 ch = 0.0f;
  f32 walkable_height = 0.0f;
  f32 walkable_radius = 0.0f;
  f32 walkable_climb = 0.0f;
  u32 nvp = 0;
  u32 vert_count = 0;
  u32 poly_count = 0;
  u32 detail_mesh_count = 0;
  u32 detail_vert_count = 0;
  u32 detail_tri_count = 0;
  u32 region_count = 0;
  u32 portal_count = 0;
};

// The decoded form. Every section is copied out rather than pointed into the buffer: the arrays
// are u16 and f32 views of a `Vector<u8>`, and reading them in place would be an aliasing and
// alignment bet for no measurable gain — a decode is one memcpy per section and happens once per
// tile per bake, against a voxel pipeline that costs milliseconds.
struct TileMesh {
  TileHeader header;
  Vector<u16> verts;          // 3 per vertex, in cells from header.bmin
  Vector<u16> polys;          // 2 * nvp per polygon: indices then neighbours
  Vector<u16> flags;          // 1 per polygon
  Vector<u8> areas;           // 1 per polygon
  Vector<u16> region;         // 1 per polygon: the connected component it belongs to
  Vector<u32> detail_meshes;  // 4 per polygon
  Vector<f32> detail_verts;   // 3 per detail vertex, world space
  Vector<u8> detail_tris;     // 4 per detail triangle
  Vector<TileRegionInfo> regions;
  Vector<TilePortal> portals;
};

void encode_tile(const TileMesh& mesh, Vector<u8>& out);
bool decode_tile(std::span<const u8> bytes, TileMesh& out);
// The header alone, for a caller that only wants the summary (grid, coordinate, counts).
bool decode_tile_header(std::span<const u8> bytes, TileHeader& out);

}  // namespace engine::nav::detail
