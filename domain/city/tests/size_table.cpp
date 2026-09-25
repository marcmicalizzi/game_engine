// Size table for domain/city (ADR-0019). The plan's records are held for the whole island — some
// ten thousand lots, a thousand blocks and a few thousand segments for a 4 km island — and read by
// every tile query; a building's spaces, walls and openings are made per lot materialized, tens of
// thousands of them for a tower, so their strides are what a district's worth of buildings costs.
#include <core/base/size_table.h>
#include <domain/city/building.h>
#include <domain/city/fragment.h>
#include <domain/city/plan.h>

using namespace engine;

// id, block, rect (16), street, district, front, use, archetype, park, civic, a spare byte.
ENGINE_EXPECT_SIZE(36, 4, city::Lot);
// id, class and three flags, line, from, to, width, superblock, a spare word.
ENGINE_EXPECT_SIZE(32, 4, city::Street);
// Street, two nodes, class and three spare bytes.
ENGINE_EXPECT_SIZE(16, 4, city::Segment);
ENGINE_EXPECT_SIZE(8, 4, city::Node);
// id, district, superblock, two rects (32), park, civic, two pin flags, first lot and count, four
// side streets (16).
ENGINE_EXPECT_SIZE(72, 4, city::Block);
// id, kind, pinned, superblock count, the seed cell's centre, the area on 64 bits.
ENGINE_EXPECT_SIZE(24, 8, city::District);
ENGINE_EXPECT_SIZE(32, 4, city::Park);
ENGINE_EXPECT_SIZE(16, 8, city::TileEntry);
// A building's records.
ENGINE_EXPECT_SIZE(28, 4, city::Space);
ENGINE_EXPECT_SIZE(32, 4, city::Wall);
ENGINE_EXPECT_SIZE(20, 4, city::Opening);
ENGINE_EXPECT_SIZE(24, 4, city::Zone);
ENGINE_EXPECT_SIZE(40, 4, city::Unit);
ENGINE_EXPECT_SIZE(24, 4, city::Core);
ENGINE_EXPECT_SIZE(24, 4, city::Floor);
ENGINE_EXPECT_SIZE(8, 4, city::Link);
// Position and size (24), lot, mesh, a spare byte, floor.
ENGINE_EXPECT_SIZE(32, 4, city::Proxy);
