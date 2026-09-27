#pragma once

// The terrain capability's ground provider, "dunes" (docs/subsystems/terrain.md, "Where it
// attaches"; docs/subsystems/scene_gen.md; ADR-0046): a scene whose terrain names it
// (`engine.scene.Terrain.provider`, or `generator: Dunes`) is drawn from this capability's dune
// field, through the registry, by a renderer that links nothing of it.
//
// The provider is the field at the entry's `time` with the entry's ridges and basins added
// (scene_gen/terrain_features.h, the waves' own arithmetic): its height, its floor (what ruins
// stand on, which the dunes migrate over), a grid of itself and the same grid at another game time
// in the 64 x 64 blocks the time-lapse spreads over its jobs, how far its fastest band travels
// between two times, and the rings round a camera (rings.h). It was the renderer's `terrain.cpp`
// under `ENGINE_RENDERER_TERRAIN` until the registry existed; the arithmetic moved, it did not
// change.

#include <domain/scene_gen/scene_gen.h>
#include <domain/terrain/dunes.h>

#include <schemas/scene.h>

namespace engine::terrain {

// The name the provider registered under.
inline constexpr const char* k_ground_provider = "dunes";

// The dune field a scene's terrain entry describes: its seed, dune height and wavelength, wind,
// ridges and basins in millimetres, and its band table when it has one. What the provider is built
// from; exposed so a caller can hold the same field (a test, a report).
FieldDesc field_desc_of(const scene::Terrain& entry);

// The field a ground this provider made holds, or null for another provider's ground.
const DuneField* dune_field(const scene_gen::GroundProvider& ground) noexcept;

}  // namespace engine::terrain
