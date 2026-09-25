// Size table for systems/world (ADR-0019). The ring keeps a tile as a 64-bit key and a ring byte;
// what crosses to the consumers and into the world log's arrays is a `TileEvent`, one per change.
#include <core/base/size_table.h>
#include <systems/world/tile_ring.h>

using namespace engine;

ENGINE_EXPECT_SIZE(8, 4, world::TileCoord);
// Tile, kind, from, to and a named zero byte: its bytes are what the determinism test compares.
ENGINE_EXPECT_SIZE(12, 4, world::TileEvent);
