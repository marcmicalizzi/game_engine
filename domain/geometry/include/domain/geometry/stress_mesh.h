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

}  // namespace engine::geometry
