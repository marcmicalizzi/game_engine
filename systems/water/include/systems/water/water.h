#pragma once

// The water capability (ADR-0027; docs/subsystems/water.md; roadmap S63; plan 05 §5.15 "Fluids and
// water surfaces"): a scene's water bodies (`engine.scene.WaterBody`) as surfaces the renderer
// draws and a walker wades in.
//
// **What it owns.** A body's shape and level, read and checked once (`Body`, `read_body`); the
// surface as a mesh with the depth under every vertex (`build_mesh`, `mesh_glb`), written once
// under a name that is the hash of its bytes and clustered by the renderer's derived-data cache
// like any mesh; the depth at a point (`depth_at`), which is what a walker wades by; and the
// placement generator "water" (src/scene_generator.cpp) the renderer's scene reader expands a
// scene's `water` block through. **What it does not.** It shades nothing: the water material is a
// class of the renderer's material table and the resolve's water branch (`gfx::k_material_water`,
// domain/gfx/shaders/water.slang), which are not capabilities, and the renderer packs a body's
// shading numbers itself (`renderer::SceneWater`). Nothing here simulates: the waves are a
// function of position and time drawn by the resolve, and refraction of what is under the surface,
// swimming, buoyancy and a ripple map are stage 2 (roadmap S86).
//
// Registration points (ADR-0027 decision 2): a placement generator (scene_gen.h), registered from
// src/scene_generator.cpp — the only one; no schema of its own (the block is the scene format's),
// no scheduler entry (nothing ticks), no render pass (the surface is an instance), no protocol
// method, no tunable. Determinism: every number is a function of the block and the ground, in f32
// and f64 + - * / and sqrt — the circle's points are a polynomial sine of this file's own, not the
// C library's — so the mesh is the same bytes from every toolchain.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/scene.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::water {

// The placement generator's registered name, which the renderer's scene reader asks the registry
// for (systems/renderer/src/scene_water.cpp).
inline constexpr std::string_view k_generator = "water";

// The mesh builder's version: part of every mesh file's bytes, so a change that makes an old file
// wrong names a new one.
inline constexpr u32 k_mesh_version = 1;

// **A body that fills** (the block's `shape.fill`; water.md, "A body that fills"; roadmap S97): the
// lattice its flood ran on, about the seed. Node (i, j) stands at body-local (origin.x + i spacing,
// origin.y + j spacing), row j along +z, and `value[j * nx + i]` is the level less the ground
// there where the node is in the flooded region (> 0), and at most `-k_fill_outside` where it is
// not — the ground at or above the level, or a hollow below it the flood did not reach. The region
// is the nodes that are positive; its edge is where the bilinear value crosses zero, which is what
// the mesh, `inside_distance` and `depth_at` all read, and `loops` are that edge traced (marching
// squares) and simplified to a quarter of the spacing: `loop_starts[k]` the first point of loop k,
// the points metres from the centre in (x, z).
inline constexpr f32 k_fill_outside = 1e-3f;
struct FillRegion {
  f32 spacing = 0.0f;
  Vec2 origin{};
  u32 nx = 0;
  u32 nz = 0;
  Vector<f32> value;
  u32 inside = 0;     // nodes in the region
  u32 crossings = 0;  // lattice edges the region's edge crosses: the mesh's boundary vertices
  Vector<Vec2> loop_points;
  Vector<u32> loop_starts;
};

// One body as this capability uses it: the block's shape about its centre, metres.
struct Body {
  std::string name;
  f64 level_m = 0.0;
  // The circle's centre, or the polygon's centroid: x and z, y the level. What the mesh is measured
  // from and its instance stands at (ADR-0053).
  WorldPos center{};
  // A circle's radius; 0 for a polygon.
  f32 radius_m = 0.0f;
  // A polygon's points less the centre, (x, z), counter-clockwise about +y (x towards -z); empty
  // for a circle.
  Vector<Vec2> outline;
  f32 shore_m = 2.0f;
  // **Open water** (the block's `open`; water.md, "To the horizon"): the outline is the near shore
  // of a sea that goes on past it wherever the ground is below the level. The mesh and the depth
  // map are the closed body's, byte for byte; what differs is what is drawn past the outline (the
  // renderer's) and where `depth_at` finds water.
  bool open = false;
  // A body that fills: its centre is the seed (x and z, y the level), and `region` is empty until
  // `fill_body` has flooded the ground, which needs the ground and so is not `read_body`'s.
  bool fill = false;
  f32 fill_extent_m = 0.0f;
  FillRegion region;
  // The wet band and the foam line (the block's `wet_m` and `foam_m`; water.md, "The shore").
  f32 wet_m = 0.3f;
  f32 foam_m = 0.6f;
  // The swash's (the block's `wind_m_s`, `fetch_km`, `tide_m` and `drain_s`; water.md, "The
  // shore"; roadmap S94): the renderer's GPU scene turns them into the shore record. `tide_m` is
  // an open body's alone (a closed body's reads 0 here); the depth map's bank covers it.
  f32 wind_m_s = 0.0f;
  f32 fetch_km = 100.0f;
  f32 tide_m = 0.0f;
  f32 drain_s = 20.0f;
  bool circle() const noexcept { return !fill && outline.empty(); }
  bool filled() const noexcept { return fill && region.nx > 0; }
};

// The block, checked: exactly one shape, a positive radius, a polygon of at least three points
// star-shaped from its centroid, finite numbers, the shading numbers in range. False with a
// sentence (no path; the caller adds where it is).
bool read_body(const scene::WaterBody& block, Body& out, std::string* error);

// Whether (x, z), metres from the body's centre, is inside its outline, and how far inside: the
// distance to the edge along the ray from the centre, scaled as the mesh's rings are, which for a
// circle is exact. Negative outside.
f32 inside_distance(const Body& body, f32 x, f32 z) noexcept;

// **The depth at a point**: the deepest of the bodies whose outline holds the point's x and z — or,
// for an open body, wherever the point is (its sea goes on past the outline) — of its level less
// the ground's surface there, never below zero; 0 outside every body. A function of
// the point and the scene alone, so a walk that asks it replays bit for bit. `ground` absent: the
// ground is at 0.
f32 depth_at(std::span<const Body> bodies, const scene_gen::Ground& ground, WorldPos at) noexcept;

// **The surface as a mesh** (docs/subsystems/water.md, "The mesh"): the outline sampled every
// `spacing` metres along its edge, and copies of it scaled towards the centre, `rings` of them
// `spacing` apart along the ray through each sample, closed by a fan at the centre — so every ring
// has the edge's vertex count, the strips between two rings are quads, and a vertex's distance from
// the edge is exact for a circle. Positions are metres from the body's centre at height 0 (the
// instance stands at the level), the normal is +y, and the UV is (the distance from the edge, the
// depth under the vertex), metres — the depth the level less the ground there, clamped at zero
// where the ground stands above the water. Triangles face +y.
struct Mesh {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  f32 spacing = 0.0f;  // metres between edge samples, and between rings along a ray
  u32 edge = 0;        // vertices a ring
  u32 rings = 0;       // rings, the edge included; the centre is one vertex more
  f32 max_depth = 0.0f;
};
// The vertex budget a body's mesh keeps under: the spacing grows until it fits.
inline constexpr u32 k_max_vertices = 65536;
// The spacing a body asks for: a third of its shore, within [0.25, 1] m.
f32 spacing_for(const Body& body) noexcept;
// A body that fills is meshed by `build_fill_mesh` (below) from its region; an unflooded one has
// an empty mesh. The circle and the polygon are rings to a fan, the bytes they always were.
void build_mesh(const Body& body, const scene_gen::Ground& ground, Mesh& out);

// **The flood** (src/fill.cpp; water.md, "A body that fills"): the region of the ground below the
// body's level connected to its seed through the lattice's four neighbours, on a lattice at
// `spacing_for(body)` grown by a quarter until the mesh it makes keeps under `k_max_vertices`,
// within `fill_extent_m` of the seed in x and z. False with a sentence when the seed's ground is
// at or above the level, or the flood reaches the bound (the level spills out of the hollow). The
// region is a function of the ground at the lattice's nodes alone — the flood on integer indices,
// the trace in f32 + - * / — so it is the same bits at any job count and on every toolchain.
bool fill_body(Body& body, const scene_gen::Ground& ground, std::string* error);
// The region's bilinear value at (x, z), metres from the centre: about the depth inside the region
// (positive), at most zero outside it and outside the lattice; -1 for a body not yet flooded.
// `inside_distance` answers this for a body that fills.
f32 fill_value(const Body& body, f32 x, f32 z) noexcept;
// **The general mesher**: the lattice's cells over the region, a cell whose four nodes are inside
// two triangles and a cell the edge crosses clipped to the edge by marching squares (its inside
// polygon, the edge's points on the lattice's edges where the bilinear value is zero, fanned), so
// the mesh covers the region and nothing above the level at the nodes, and two cells that share a
// lattice edge share its vertices. A saddle is joined when the cell's mean is inside. The UV is
// (the distance from the edge, by a chamfer over the nodes; the depth under the vertex), as the
// rings' is.
void build_fill_mesh(const Body& body, const scene_gen::Ground& ground, Mesh& out);

// The mesh as a GLB: positions, normals, TEXCOORD_0 and indices, one primitive with one material
// named "water" whose base colour is `shallow` and roughness `roughness` — what stage 1 drew, and
// what every reader that is not the resolve's water branch still draws.
Vector<u8> mesh_glb(const Mesh& mesh, Vec3 shallow, f32 roughness);
// The file a mesh is written to on its own: "water-<hash of its bytes>.glb". A body's mesh and
// depth map go under `body_file_stem` instead, which covers both.
std::string mesh_file_name(std::span<const u8> glb);

// **The depth map** (docs/subsystems/water.md, "The depth map"): the depth under the body on a
// square grid over its bounding square, from the same `Ground::surface` the vertices sample, at a
// quarter of the mesh's spacing (`size` within [k_depth_map_min, k_depth_map_max] texels a side).
// The mesh's vertex depths draw the ground as a piecewise-linear surface over its rings and fan,
// whose facets the eye sees through the water (rings and trapezoids in the bottom's colour); the
// resolve samples this map bilinearly instead, so the bottom it shades follows the ground and not
// the triangles. Texel (i, j)'s centre is body-local (origin.x + i texel_m, origin.y + j texel_m),
// row j along +z; its depth is **signed** (version 2, roadmap S94): `texels[j * size + i] / 65535
// * (depth_max + bank_max) - bank_max`, metres (`depth_map_depth`) — positive under the level, and
// on the bank the ground's height above the level as a negative depth, down to `-bank_max`, so the
// shore's slope is known on both sides of the waterline and the swash knows how high the bank
// stands. 16 bits over the body's own range: a 2.3 m pool with a 2 m bank keeps 0.07 mm. A
// version-1 file (the bank clamped at zero) is read with `bank_max` 0, as before.
inline constexpr u32 k_depth_map_version = 2;
inline constexpr u32 k_depth_map_min = 64;
inline constexpr u32 k_depth_map_max = 1024;
// How high a bank the map stores: the larger of 2 m and twice the tide plus a metre, so the swash
// and the tide's range are in it and a hill beside a pool does not spend the 16 bits.
inline constexpr f32 k_depth_map_bank_min = 2.0f;
struct DepthMap {
  u32 size = 0;          // texels a side; 0 is no map
  Vec2 origin{};         // body-local (x, z) of texel (0, 0)'s centre, metres
  f32 texel_m = 0.0f;    // metres between texel centres
  f32 depth_max = 0.0f;  // the deepest texel's depth
  f32 bank_max = 0.0f;   // the highest bank stored, metres above the level (0: version 1)
  Vector<u16> texels;    // size * size, rows along +z
};
// Texel (i, j)'s signed depth, metres.
f32 depth_map_depth(const DepthMap& map, u32 i, u32 j) noexcept;
void build_depth_map(const Body& body, const Mesh& mesh, const scene_gen::Ground& ground,
                     DepthMap& out);
// The map as a file: a 32-byte header — "EWDM", the version, the size, bank_max (version 1: 0),
// origin x and z, texel_m and depth_max as little-endian u32 and f32 — then the texels as
// little-endian u16. Versions 1 and 2 are read. The
// renderer's scene reader reads it (systems/renderer/src/scene_water.cpp; the layout is mirrored
// there, since the renderer may not call a capability, and `water_scene_tests.cpp` holds the two
// to each other).
Vector<u8> depth_map_bytes(const DepthMap& map);
bool read_depth_map(std::span<const u8> bytes, DepthMap& out, std::string* error);
// The name both of a body's files take, "water-<hash of the GLB's bytes and the map's>": the mesh
// is `<stem>.glb` and the map `<stem>.depth`, beside it, so the one key covers both and the reader
// finds the map from the mesh's path.
std::string body_file_stem(std::span<const u8> glb, std::span<const u8> map);

// The generator's parameters, as the reader hands them: `{"bodies": [<WaterBody>...]}`, of which at
// most one is open (a scene's sea is one plane at one level; `open` refuses a second with a
// sentence). Its `expand` makes one mesh and one placement per body, in the block's order — mesh k
// and instance k are body k, which is the contract the renderer's reader names a body's material by
// — the instance at the body's centre and level, unrotated.
JsonValue generator_params(std::span<const scene::WaterBody> bodies);

// A sine and cosine in f64 from + - * / alone (a range reduction and a polynomial), so the circle's
// points are the same bits on every toolchain; within 1e-15 of the true values.
void sin_cos(f64 radians, f64& s, f64& c) noexcept;

}  // namespace engine::water
