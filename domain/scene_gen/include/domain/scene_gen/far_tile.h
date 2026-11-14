#pragma once

// **A far tile** (G198; scene_gen.md, "A far tile"; renderer.md, "The far tile"): one mesh that
// stands for the far forms of several placed meshes — a tile's talus, a field of boulders —
// handed by a generator (or the world) as a recipe like any tile mesh, built by the host through
// the derived-data cache it builds every tile mesh through, and drawn as **one** instance: one
// run of pairs and one structure where the meshes it stands for were one each.
//
// **What the recipe holds.** Its members — each a `PlacementMesh` exactly as a near tile hands it
// (a recipe's bytes, or a file's path and hash), so the host builds or finds the same near entry
// and reads its far form (`geometry::FarForm`, carried in the member's container or baked from its
// finest level, the same bytes either way) — and where each stands from the far tile's own origin:
// whole millimetres, a turn and a scale. And which far form level every member draws (`level`,
// clamped to what the member has). Nothing in it names a generator but the members' own paths, so
// an imported boulder scattered by a scene file and a solver's talus make the same far tile.
//
// The file name is `<anything>.far.json`: the mesh generator "far", which the renderer answers
// itself (it needs the members' meshes, which only a host can build).

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/scene_gen/scene_gen.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::scene_gen {

inline constexpr const char* k_far_tile_generator = "far";
inline constexpr u32 k_far_tile_version = 1;

// One member standing in the far tile: its mesh (an index of `FarTile::meshes`), its place from the
// far tile's origin in whole millimetres, and its turn and scale.
struct FarTilePlacement {
  u32 mesh = 0;
  i64 x_mm = 0;
  i64 y_mm = 0;
  i64 z_mm = 0;
  Quat rotation = Quat::identity();
  Vec3 scale = Vec3::one();
};

struct FarTile {
  u32 level = 0;  // the far form level every member draws (geometry::FarForm::levels)
  Vector<PlacementMesh> meshes;
  Vector<FarTilePlacement> placements;
};

// The recipe's bytes (canonical: the same far tile is the same bytes) and the `PlacementMesh` that
// names it: its path `<dir>/far-<hash>.far.json`, its name `far/<hash>`, its reach. False, with a
// sentence, for a far tile with no placement or a placement naming a mesh it does not list.
bool far_tile_mesh(const FarTile& tile, std::string_view dir, f32 reach, PlacementMesh& out,
                   std::string* error = nullptr);
// Reads a recipe `far_tile_mesh` wrote. False, with a sentence, for anything else.
bool read_far_tile(std::string_view recipe, FarTile& out, std::string* error = nullptr);
// Whether a tile mesh is a far tile (its path names the generator "far").
bool is_far_tile(const PlacementMesh& mesh) noexcept;

}  // namespace engine::scene_gen
