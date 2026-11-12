#pragma once

// **The crowns' priors of a scene** (docs/subsystems/renderer.md, "Indirect light: the world-space
// cache", "Stage 7"; roadmap R69 stage 7): for every mesh of a loaded scene that has a crown — its
// finest level holds alpha-tested leaf cards standing `gfx::k_crown_min_height_m` tall or more —
// the grid of its own shade (`gfx::build_crown_prior`, crown_prior.h), from the mesh's own leaves.
// A distance form reads its source's grid. One table: a `gfx::CrownPriorDesc` a mesh, then the
// grids, which is what the resolve's ambient
// and the cache's sample pass read through `ResolveParams::gi_crowns`.
//
// A mesh that arrives after the load (a tile's, renderer.md "Meshes that come and go") has no
// record: its index is past the table's count, and its foliage is read as everything else is.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/crown_prior.h>
#include <domain/gfx/resources.h>
#include <systems/renderer/scene.h>

#include <string>

namespace engine::renderer {

// What the scene's crowns came to, for the load's log line and a page's table.
struct CrownPriorTable {
  Vector<u8> bytes;  // the records, then the grids; empty when no mesh has a crown
  u32 meshes = 0;    // the records: every mesh of the scene
  u32 crowns = 0;    // of them, with a grid of their own
  u32 forms = 0;     // ... and distance forms that read their source's
  u32 plain = 0;     // ... and meshes of low leaves: the one cell of no shade (stage 8)
  u32 leaf_triangles = 0;
  f64 leaf_area_m2 = 0.0;    // over every crown, one-sided
  f64 leaf_volume_m3 = 0.0;  // the cells that hold a leaf
  f32 min_up = 1.0f;         // the deepest shade any crown's leaves stand in
};

// Builds the table from the scene's merged meshes: level 0 of each (the source geometry), the
// clusters whose material is alpha-tested as the leaves. A scene whose float positions are gone
// (a streamed one) has no crowns.
void build_crown_priors(const SceneData& data, CrownPriorTable& out);

// The table on the device, or nothing where no mesh has a crown (`count` 0, `buffer` untouched).
bool upload_crown_priors(const gfx::Device& device, const SceneData& data,
                         gfx::BufferResource& buffer, u32& count, std::string* error);

}  // namespace engine::renderer
