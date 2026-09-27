#pragma once

// The ruins as a placement generator, "ruins" (docs/subsystems/ruins.md, "Where it runs";
// docs/subsystems/scene_gen.md; ADR-0046): a scene's `ruins` entry, or a `placements` entry naming
// "ruins", is expanded by this capability through the scene-generator registry, by a scene reader
// and a world ring that link nothing of it.
//
// The entry's parameters are an `engine.scene.RuinScatter` (a kit, a seed, a tile size, a square of
// tiles and a count or a density, the wind, and a representation). `open` reads the kit (and the
// block kit when the entry is drawn in blocks) and chooses the buildings' tiles once; `expand` is
// the whole entry — every building assembled standing on the ground it is handed, on the job pool
// past 32 tiles, joined in tile order — in sections (the kit's members) or blocks (the block kit's
// meshes only, since nothing would instance the members); `meshes` is what a streamed scene holds
// resident, the kit's members always and the block kit's when the entry is drawn in blocks; and
// `tile` is one tile's building in the representation its ring draws (the scene's
// `engine.scene.WorldRing::ruins`: blocks, sections with debris, walls alone), exactly the building
// `expand` puts on that tile. A placement's tag is 1 for rubble — a debris member, a fallen block —
// and its `kind` the representation drawn: 0 blocks, 1 sections, 2 walls.
//
// Registered from this capability's own source (`scene_generator.cpp`); the module is
// `WHOLE_ARCHIVE`, which keeps the registrar. It was the renderer's `scene.cpp` (`expand_ruins`,
// `stream_ruins`, under `ENGINE_RENDERER_RUINS`) and the world's `ruins_tiles.cpp` (under
// `ENGINE_WORLD_RUINS`); the calls moved, the assembler did not change.

#include <core/base/types.h>

namespace engine::ruins {

// The name the generator registered under.
inline constexpr const char* k_placement_generator = "ruins";

// The representations a tile is drawn in, the placements' `kind`.
inline constexpr u8 k_drawn_blocks = 0;
inline constexpr u8 k_drawn_sections = 1;
inline constexpr u8 k_drawn_walls = 2;

}  // namespace engine::ruins
