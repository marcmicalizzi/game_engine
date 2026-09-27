#pragma once

// The scene's generator consumers in their declared order (docs/subsystems/world.md, "The order";
// scene_gen.md, "The order rule"; ADR-0046 decision 3, ADR-0040 decision 2): **the ground first,
// then the placements**, the placement entries in the scene's order inside the one placements
// consumer. Activation and a move between rings call consumers in registration order, so a tile's
// ground is built before anything that stands on it; deactivation calls them in reverse, so the
// placements go before their ground. The scene reader keeps the same order when it reads a scene
// whole: the terrain's provider first, then every entry in the file's order.
//
// A host registers the document and store consumers, where it has them, **before** these: the
// ground's tile record is then written on deactivation before the store consumer snapshots the tile
// (the snapshot carries every projection of the tile), and on activation it is read after the
// store's reconciliation. The one function is the order, so no host spells it.

#include <systems/world/ground_tiles.h>
#include <systems/world/placement_tiles.h>
#include <systems/world/world.h>

namespace engine::world {

// Registers `ground` (null: the scene's ground has no tiles, or the host holds none of them) and
// then `placements` (null: the scene streams no placement entry) with `world`, in that order, and
// points the placements' pair budget at the world's ring. Both must outlive the world's use of
// them.
void add_scene_consumers(World& world, GroundTiles* ground, PlacementTiles* placements);

}  // namespace engine::world
