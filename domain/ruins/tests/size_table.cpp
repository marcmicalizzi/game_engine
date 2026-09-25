// Size table for domain/ruins (ADR-0019). The assembler's output records are its hot types: a
// thousand ruins are some fifty thousand `Instance`s, written once per tile materialization and
// read once into the scene's instance table, so their stride is what a desert's worth of them
// costs to hold and to hash.
#include <core/base/size_table.h>
#include <domain/ruins/assembler.h>
#include <domain/ruins/blocks.h>

using namespace engine;

// Position (12), member and building (8), wall, slot and height (6), kind and yaw (2): no padding.
ENGINE_EXPECT_SIZE(28, 4, ruins::Instance);
// The block layer's record, the same stride: position (12), block and building (8), wall and
// index (4), course, role, yaw and flags (4). A thousand ruins laid block by block are some
// hundreds of thousands of them.
ENGINE_EXPECT_SIZE(28, 4, ruins::Block);
// Three 2-vectors (24), height and reach (8), building (4), wall, windward and a spare byte (4).
ENGINE_EXPECT_SIZE(40, 4, ruins::Drift);
// Seed (8), tile (8), origin (12), four counts (16), walls, shape and yaw (4).
ENGINE_EXPECT_SIZE(48, 8, ruins::Site);
// A rubble site: its centre and place along the wall (12), wall and index (4), yaw, side and two
// spare bytes (4). A building has a few dozen, both representations read them.
ENGINE_EXPECT_SIZE(20, 4, ruins::RubbleSite);
