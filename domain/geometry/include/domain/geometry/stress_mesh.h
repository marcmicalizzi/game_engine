#pragma once

// Procedural meshes that are **deliberately hard for the cluster builder**, and the textures that
// make what the builder did to them visible (docs/subsystems/geometry.md, "The shredded atlas").
//
// Why this is engine code and not a test helper: the same generator has to be reachable from
// three places that cannot share a test header — this module's unit tests, the renderer's
// procedural scene (so a picture of it can be compared against the reference path tracer), and
// the versioned scene corpus under `content/test-scenes` that CI runs. It is also the only way
// to keep the fixture free of a third-party asset, which matters here because the defect it
// guards was found on a model nobody may commit.
//
// The mesh it builds is a torus whose UV atlas has been shredded into thousands of small islands
// scattered across the atlas, with the seam vertices duplicated exactly as an exporter writes
// them. That is the shape of an atlas an AI mesh generator or a photogrammetry scan produces,
// and it is the case that broke LOD simplification: a collapse that crosses an island edge makes
// every triangle over the join interpolate its texture across two unrelated parts of the atlas.
// A torus rather than a sphere because it has no poles, so every quad is the same size and the
// parametrization is periodic in both directions — the wrap is itself a seam, for free.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster.h>

namespace engine::geometry {

struct ShreddedAtlasOptions {
  u32 segments = 128;  // quads around the major circle
  u32 rings = 64;      // quads around the tube
  f32 major_radius = 1.0f;
  f32 minor_radius = 0.35f;
  // Quads along one side of an atlas island. 1 makes every quad its own island, which no
  // simplifier can do anything with at all; 4 leaves an island's interior weldable and only its
  // border a seam, which is what a real fragmented atlas looks like.
  u32 island_side = 4;
  // The atlas is a grid of this many slots a side. Raised automatically when the islands do not
  // fit; a slot is left empty when there are more slots than islands.
  u32 atlas_cells = 0;  // 0: the smallest square that holds every island
  // The share of a slot left blank around an island, so that bilinear sampling of a correct UV
  // never reaches the neighbouring slot and a wrong colour is always the simplifier's doing.
  f32 island_margin = 0.15f;
  u32 seed = 0x9e3779b9u;  // the island-to-slot shuffle; the same seed is the same atlas
};

struct ShreddedAtlasMesh {
  Vector<Vec3> positions;
  Vector<Vec3> normals;  // the exact torus normal, identical across every copy of a position
  Vector<Vec2> uvs;
  Vector<u32> indices;
  u32 island_count = 0;
  u32 atlas_cells = 0;
  // Vertices that share a position with another vertex that disagrees about the UV — the thing
  // the simplifier has to be told about. Every island border vertex is one.
  u32 seam_vertices = 0;
};

// Deterministic: the same options give byte-identical arrays, on any machine and at any thread
// count. `segments` and `rings` are rounded up to a multiple of `island_side`.
void build_shredded_atlas_torus(const ShreddedAtlasOptions& options, ShreddedAtlasMesh& out);

// The probe texture that turns a UV error into a colour error: `size` x `size` RGBA8, one
// saturated hue per atlas slot with a gentle gradient inside it. A UV that stays in its own
// island reads its island's hue; a UV that a collapse dragged into another island reads a
// completely different one, so a picture difference measures exactly the defect and not the
// shading. `atlas_cells` is `ShreddedAtlasMesh::atlas_cells`.
void build_atlas_probe_texture(u32 size, u32 atlas_cells, Vector<u8>& rgba);

// ---- the morph fixture -------------------------------------------------------------------------
//
// A UV sphere with morph channels over it, here for the same reason the shredded atlas is: the
// geometry tests, the GPU deform tests and a renderer scene all need the same mesh, and the one
// thing a morph rig cannot be tested against is a Khronos sample that may or may not be on the
// machine. Two shapes come out of one generator, and they are the two the deform chain has to be
// right on:
//
//   - **a morphing sphere**: one channel with a `falloff` wide enough to reach every vertex, so
//     the whole surface moves. That is the case where reconstructing a normal from the deformed
//     positions and interpolating a delta normal disagree the most, because the surface's
//     curvature changes everywhere.
//   - **a face-like rig**: several channels with a small `falloff`, each touching a few percent
//     of the vertices. That is the case the *storage* is designed for, and the one where the
//     per-cluster directory earns its keep — most clusters are touched by no channel at all.
//
// A channel is a Gaussian bump along the surface normal centred on a point of the sphere, with
// deltas under `cutoff` of its peak dropped, which is what makes it sparse. Normal deltas are the
// exact difference between the displaced surface's analytic normal and the rest one, so a test
// can compare a chain that carries delta normals against the truth rather than against itself.
struct MorphFixtureOptions {
  u32 segments = 64;  // quads around the equator
  u32 rings = 32;     // quads from pole to pole
  f32 radius = 1.0f;
  u32 channels = 6;
  f32 amplitude = 0.08f;  // peak displacement, as a fraction of the radius
  // The Gaussian's sigma in radians of arc. 0.25 touches a few percent of a 64x32 sphere; 4.0
  // reaches every vertex, which is the "morphing sphere" shape.
  f32 falloff = 0.25f;
  f32 cutoff = 1.0e-3f;  // deltas below this fraction of the peak are left out of the channel
  bool normals = true;   // channels carry NORMAL deltas as well as POSITION ones
  // Splits the seam ring (u = 0) so that its two sides are separate vertices carrying *different*
  // deltas, which is the authored discontinuity `weld_vertices` and `morph_seams` exist for. Off
  // makes the fixture's channels continuous everywhere.
  bool split_seam = true;
  u32 seed = 0x85ebca6bu;  // where the channels sit; the same seed is the same rig
};

struct MorphFixtureMesh {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  Vector<MorphChannelSource> morph;
  // Vertices that share a position with another vertex that disagrees about a morph delta.
  u32 morph_seam_vertices = 0;
};

// Deterministic: the same options give byte-identical arrays on any machine.
void build_morph_sphere(const MorphFixtureOptions& options, MorphFixtureMesh& out);

// The fixture's own answer for one vertex at one set of weights, in float: rest position plus the
// sum of weight times delta, and the same for the normal (renormalized). It is what the CPU
// reference of the morph stage is checked against, so that the test measures the *format* —
// quantization and the sparse layout — rather than comparing the code with itself.
Vec3 morph_fixture_position(const MorphFixtureMesh& mesh, u32 vertex,
                            std::span<const f32> weights) noexcept;
Vec3 morph_fixture_normal(const MorphFixtureMesh& mesh, u32 vertex,
                          std::span<const f32> weights) noexcept;

}  // namespace engine::geometry
