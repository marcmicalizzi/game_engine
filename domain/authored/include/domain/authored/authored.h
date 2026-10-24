#pragma once

// The `authored` placement generator (docs/subsystems/authored.md; scene_gen.md; ADR-0046): a
// scene's own static instances, a tile at a time. A streamed world used to leave a scene file's
// `instances` in the scene's fixed prefix — every cluster of every tree a pair however far it stood
// — while only a generator's placements came and went with their tiles, which is what put the full
// v8 forest past the visibility id's 2^24 pairs before its first frame. The scene reader now hands
// the instances over as this generator's entry (`engine.scene.AuthoredSet`) when the world streams
// them (renderer.md, "Authored instances per tile"), and the world's placements consumer streams
// them like any other entry.
//
// **The rules.** An instance is filed under the tile its origin is on (`anchor_tile`), however far
// its mesh reaches: the reader has already cut every form its rings draw to the nearest a camera
// can be to an origin on a tile of that ring, every sphere's offset from the origin counted, so a
// crown that reaches into a nearer tile is drawn in the form its own origin allows and no tile
// needs to know its neighbours'. A tile's placements are its instances in the order of their ids,
// each drawn in its form set's mesh for the tile's ring (the last ring's past the end); the id is
// the instance's place in the scene file's own list plus one, so it is the same whichever tiles
// came in first and on every reload. Nothing here reads the ground, the seed or the earlier
// entries: the set is the scene's, already standing where the file stood it.

#include <core/base/types.h>
#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>

namespace engine::authored {

inline constexpr const char* k_generator = "authored";
// ADR-0027's stance: a tile's placements are a function of the set and the tile alone.
inline constexpr const char* k_determinism = "derived";

// The tile an origin is filed under: (floor(x / tile_size), floor(z / tile_size)). False for an
// origin no i32 tile reaches (not finite, or past 2^31 tiles), which `open` refuses by index.
bool anchor_tile(const WorldPos& origin, f32 tile_size, scene_gen::TileCoord& out) noexcept;

}  // namespace engine::authored
