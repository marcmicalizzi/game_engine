#pragma once

// A scene terrain's fixed features (`engine.scene.Terrain`'s `ridges` and `basins`;
// docs/subsystems/scene_gen.md, "The fixed features"): rock ridges along segments and bowls round
// points, placed by hand so a camera path can be laid out against them, which every ground provider
// adds to its own field the same way — the renderer's waves multiply their dunes down over them and
// add them, the terrain capability's dunes thin their sand over them and add them.
//
// **Why here, in the registry's module.** They are part of the terrain *entry*, not of any one
// provider, and two providers in two modules that must not depend on each other add them: the
// arithmetic lives beside the entry's type so there is one copy of it, and a ridge is the same rock
// whichever ground it stands in. It was the renderer's (`terrain.cpp`) until the dunes' provider
// moved into the terrain capability; the expressions are the same ones, in the same order, so every
// height they enter is the same float it was.

#include <core/base/types.h>

#include <schemas/scene.h>
#include <span>

namespace engine::scene_gen {

// What the ridges and basins are at a point: their heights, apart — the waves add them one after
// the other, the dunes as one sum — and the masks a provider's own field is shaped by: `ridge_mask`
// the largest ridge profile under the point, `flatten` the least of the basins' flattening (1
// outside every basin).
struct TerrainFeatures {
  f32 ridges = 0.0f;
  f32 basins = 0.0f;
  f32 ridge_mask = 0.0f;
  f32 flatten = 1.0f;
};
TerrainFeatures terrain_features(u32 seed, std::span<const scene::Ridge> ridges,
                                 std::span<const scene::Basin> basins, f32 x, f32 z) noexcept;

// How much of each feature is under (x, z), in [0, 1]: the largest ridge profile and the largest
// basin weight. What the renderer's surface map is chosen from.
f32 ridge_weight(std::span<const scene::Ridge> ridges, f32 x, f32 z) noexcept;
f32 basin_weight(std::span<const scene::Basin> basins, f32 x, f32 z) noexcept;

}  // namespace engine::scene_gen
