#pragma once

// **The far tile** (G198; renderer.md, "The far tile"; geometry.md, "The far form"; scene_gen's
// far_tile.h): a tile mesh whose recipe names other placement meshes and where each stands, built
// as **one** rigid mesh of their far forms — each member built or found exactly as a near tile
// builds it (`load_placement_mesh`, the same derived-data entry), its far form read from its
// container or baked from its finest level (the same bytes), turned, scaled and moved to its place,
// and the whole clustered with its own DAG, one part a distinct material. So a ring past the first
// draws a tile's talus as one instance of a few clusters where the near ring drew forty meshes of a
// thousand each, and the meshes it stands for need not be resident.
//
// `load_placement_mesh` answers a far tile through this; a caller that wants the numbers calls it.

#include <core/base/types.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>

#include <string>

namespace engine::renderer {

struct FarTileStats {
  u32 members = 0;        // distinct meshes it stands for
  u32 placements = 0;     // their placements
  u32 baked = 0;          // members whose container carried no far form (baked at load)
  u32 materials = 0;      // its parts
  u32 triangles = 0;      // its finest level
  u32 clusters = 0;       // every level: the pairs one instance of it takes
  u32 source_triangles = 0;  // the members' finest levels, summed over the placements
  f32 error_m = 0.0f;     // the largest honest error of a member's level, times its scale
  f64 members_ms = 0.0;   // loading the members
  f64 build_ms = 0.0;     // the merge and the DAG
};

// Builds the far tile `mesh` names (`scene_gen::is_far_tile`). False, with a sentence, for a recipe
// that does not read, a member that cannot be built, or a member with no far form (a deforming
// mesh).
bool load_far_tile(const scene_gen::PlacementMesh& mesh, const PlacementMeshOptions& options,
                   PlacementMeshData& out, std::string& error, FarTileStats* stats = nullptr);

}  // namespace engine::renderer
