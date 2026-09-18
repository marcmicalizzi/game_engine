#include "tile_format.h"

#include <core/base/assert.h>

#include <cstring>
#include <type_traits>

namespace engine::nav::detail {
namespace {

constexpr usize align4(usize n) noexcept { return (n + 3u) & ~usize{3}; }

// Every section is a flat copy of trivially copyable elements. The assertion is the contract: a
// type that stops being trivially copyable stops being storable this way, and should say so here
// rather than in a GCC diagnostic about memcpy on a non-trivial type.
template <class T>
void append(Vector<u8>& out, const T* data, usize count) {
  static_assert(std::is_trivially_copyable_v<T>, "tile sections are flat copies");
  if (count != 0) {
    const usize bytes = count * sizeof(T);
    const usize at = out.size();
    out.resize(static_cast<u32>(align4(at + bytes)));
    std::memcpy(out.data() + at, data, bytes);
  } else {
    out.resize(static_cast<u32>(align4(out.size())));
  }
}

// Reads `count` elements at `cursor`, advancing it past the padded section. False when the
// buffer is too short, which is the only way a decode fails after the magic matched.
template <class T>
bool read(std::span<const u8> bytes, usize& cursor, Vector<T>& out, usize count) {
  static_assert(std::is_trivially_copyable_v<T>, "tile sections are flat copies");
  out.clear();
  const usize need = count * sizeof(T);
  if (cursor + need > bytes.size()) return false;
  if (count != 0) {
    out.resize(static_cast<u32>(count));
    std::memcpy(out.data(), bytes.data() + cursor, need);
  }
  cursor = align4(cursor + need);
  return cursor <= bytes.size();
}

}  // namespace

void encode_tile(const TileMesh& mesh, Vector<u8>& out) {
  out.clear();
  out.resize(static_cast<u32>(sizeof(TileHeader)));
  std::memcpy(out.data(), &mesh.header, sizeof(TileHeader));
  out.resize(static_cast<u32>(align4(out.size())));

  append(out, mesh.verts.data(), mesh.verts.size());
  append(out, mesh.polys.data(), mesh.polys.size());
  append(out, mesh.flags.data(), mesh.flags.size());
  append(out, mesh.areas.data(), mesh.areas.size());
  append(out, mesh.region.data(), mesh.region.size());
  append(out, mesh.detail_meshes.data(), mesh.detail_meshes.size());
  append(out, mesh.detail_verts.data(), mesh.detail_verts.size());
  append(out, mesh.detail_tris.data(), mesh.detail_tris.size());
  append(out, mesh.regions.data(), mesh.regions.size());
  append(out, mesh.portals.data(), mesh.portals.size());
}

bool decode_tile_header(std::span<const u8> bytes, TileHeader& out) {
  if (bytes.size() < sizeof(TileHeader)) return false;
  std::memcpy(&out, bytes.data(), sizeof(TileHeader));
  return out.magic == k_tile_magic && out.version == k_tile_version;
}

bool decode_tile(std::span<const u8> bytes, TileMesh& out) {
  if (!decode_tile_header(bytes, out.header)) return false;
  const TileHeader& h = out.header;
  usize cursor = align4(sizeof(TileHeader));

  const usize nvp = h.nvp;
  if (nvp < 3 || nvp > 32) return false;
  if (!read(bytes, cursor, out.verts, usize{3} * h.vert_count)) return false;
  if (!read(bytes, cursor, out.polys, usize{2} * nvp * h.poly_count)) return false;
  if (!read(bytes, cursor, out.flags, h.poly_count)) return false;
  if (!read(bytes, cursor, out.areas, h.poly_count)) return false;
  if (!read(bytes, cursor, out.region, h.poly_count)) return false;
  if (!read(bytes, cursor, out.detail_meshes, usize{4} * h.detail_mesh_count)) return false;
  if (!read(bytes, cursor, out.detail_verts, usize{3} * h.detail_vert_count)) return false;
  if (!read(bytes, cursor, out.detail_tris, usize{4} * h.detail_tri_count)) return false;
  if (!read(bytes, cursor, out.regions, h.region_count)) return false;
  if (!read(bytes, cursor, out.portals, h.portal_count)) return false;
  return true;
}

}  // namespace engine::nav::detail
