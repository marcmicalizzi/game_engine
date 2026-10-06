#pragma once

// The proxy scene fragment (docs/subsystems/city.md, "The proxy fragment"): the plan and its
// buildings as an `engine.scene.Scene` of **proxy instances** — a dozen box meshes (a building's
// massing, a slab, an exterior and an interior wall, a core, a window, a road, a lane, a park, a
// plaza, a tree marker), each instanced per element and scaled to it, so a district is thousands
// of instances of a dozen meshes, which is what the renderer's instance-of-mesh model is for. It is
// how the city is looked at before any kit exists: `engine-view --scene` draws it like any scene.
//
// **And a scene generator.** The same proxies are what the placement generator "city" makes
// (scene_generator.h, ADR-0046): a scene naming `city` with a plan draws them without a fragment on
// disk, and a streamed world streams a tile's.
//
// Every proxy is axis-aligned (the plan and the buildings are), so an instance is a translation
// and a scale with no rotation, and every number in the fragment comes from integer centimetres.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/world.h>
#include <domain/city/building.h>
#include <domain/city/plan.h>

#include <schemas/scene.h>
#include <string>
#include <string_view>

namespace engine::city {

enum class ProxyMesh : u8 {
  massing = 0,
  civic = 1,
  slab = 2,
  exterior_wall = 3,
  interior_wall = 4,
  core = 5,
  window = 6,
  road = 7,
  lane = 8,
  park = 9,
  plaza = 10,
  tree = 11,
};
inline constexpr u32 k_proxy_meshes = 12;
const char* proxy_mesh_name(ProxyMesh mesh) noexcept;

// One proxy: a box standing on (x, y, z) — the centre of its bottom face — with its size along the
// world's axes, centimetres. 32 bytes.
struct Proxy {
  i32 x = 0;
  i32 y = 0;
  i32 z = 0;
  i32 sx = 0;
  i32 sy = 0;
  i32 sz = 0;
  u32 lot = k_no_id;  // the lot's id, for a building's proxies
  ProxyMesh mesh = ProxyMesh::massing;
  u8 reserved = 0;
  u16 floor = 0;
};

// What a proxy's anchor is: the centre of its bottom face, on the plan.
inline bool proxy_in(const Proxy& p, const Rect& r) noexcept {
  return p.x >= r.x0 && p.x < r.x1 && p.z >= r.z0 && p.z < r.z1;
}

// A building's proxies at `detail`: massing (one box), floors (a slab a floor, the cores, the
// exterior walls and their windows), or rooms (every wall besides). Appends.
void append_building_proxies(const Plan& plan, const Building& building, Stage detail,
                             Vector<Proxy>& out);
// The street segments, parks, plazas and trees whose anchor lies in `area`. Appends.
void append_ground_proxies(const Plan& plan, const Rect& area, Vector<Proxy>& out);
// The ground proxies of one district: the segments on its blocks' sides, its parks and trees.
void append_district_ground_proxies(const Plan& plan, u32 district, Vector<Proxy>& out);

// Everything a tile holds: the buildings of `lots_in_tile` generated at `detail` and their proxies
// with the ground's, keeping those whose anchor lies in the tile. A tile's content is the whole
// plan's content restricted to the tile, which the tests assert.
bool tile_proxies(const Plan& plan, TileCoord tile, Stage detail, Vector<Proxy>& out,
                  std::string* error);

// Where a proxy's box stands and how large it is, metres. `proxy_position` is the place in f64,
// exact from the integer centimetres anywhere: the placement the scene generator "city" makes of
// it (scene_generator.h; ADR-0053), and what a fragment writes into the scene file's `worldpos`
// `translation`. The scale is the box's own size and stays float32.
WorldPos proxy_position(const Proxy& proxy) noexcept;
Vec3 proxy_scale(const Proxy& proxy) noexcept;
// A proxy mesh's file in `dir`: "<dir>/<name>.glb".
std::string proxy_mesh_path(std::string_view dir, ProxyMesh mesh);

// A deterministic order (by position, then size, mesh and lot) for comparing two proxy sets.
void sort_proxies(Vector<Proxy>& proxies);
u64 hash_proxies(const Vector<Proxy>& proxies) noexcept;

// Writes the twelve proxy meshes as GLBs of one unit box each (a metre on a side, its bottom face
// on the origin), each its own colour, into `dir`.
bool write_proxy_meshes(const std::string& dir, std::string* error);

// The fragment: the proxy meshes (at `mesh_dir`, relative to the fragment's directory where it
// can be) and one instance per proxy, named by its lot and floor.
void make_fragment(const Vector<Proxy>& proxies, std::string_view mesh_dir,
                   std::string_view fragment_dir, std::string_view name, scene::Scene& scene);
bool write_fragment(const std::string& path, const Vector<Proxy>& proxies,
                    std::string_view mesh_dir, std::string_view name, std::string* error);

}  // namespace engine::city
