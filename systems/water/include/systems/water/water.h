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
  bool circle() const noexcept { return outline.empty(); }
};

// The block, checked: exactly one shape, a positive radius, a polygon of at least three points
// star-shaped from its centroid, finite numbers, the shading numbers in range. False with a
// sentence (no path; the caller adds where it is).
bool read_body(const scene::WaterBody& block, Body& out, std::string* error);

// Whether (x, z), metres from the body's centre, is inside its outline, and how far inside: the
// distance to the edge along the ray from the centre, scaled as the mesh's rings are, which for a
// circle is exact. Negative outside.
f32 inside_distance(const Body& body, f32 x, f32 z) noexcept;

// **The depth at a point**: the deepest of the bodies whose outline holds the point's x and z, of
// its level less the ground's surface there, never below zero; 0 outside every body. A function of
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
void build_mesh(const Body& body, const scene_gen::Ground& ground, Mesh& out);

// The mesh as a GLB: positions, normals, TEXCOORD_0 and indices, one primitive with one material
// named "water" whose base colour is `shallow` and roughness `roughness` — what stage 1 drew, and
// what every reader that is not the resolve's water branch still draws.
Vector<u8> mesh_glb(const Mesh& mesh, Vec3 shallow, f32 roughness);
// The file a mesh is written to: "water-<hash of its bytes>.glb".
std::string mesh_file_name(std::span<const u8> glb);

// The generator's parameters, as the reader hands them: `{"bodies": [<WaterBody>...]}`. Its
// `expand` makes one mesh and one placement per body, in the block's order — mesh k and instance k
// are body k, which is the contract the renderer's reader names a body's material by — the
// instance at the body's centre and level, unrotated.
JsonValue generator_params(std::span<const scene::WaterBody> bodies);

// A sine and cosine in f64 from + - * / alone (a range reduction and a polynomial), so the circle's
// points are the same bits on every toolchain; within 1e-15 of the true values.
void sin_cos(f64 radians, f64& s, f64& c) noexcept;

}  // namespace engine::water
