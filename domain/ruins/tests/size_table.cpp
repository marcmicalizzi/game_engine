// Size table for domain/ruins (ADR-0019). The assembler's output records are its hot types: a
// thousand ruins are some fifty thousand `Instance`s, written once per tile materialization and
// read once into the scene's instance table, so their stride is what a desert's worth of them
// costs to hold and to hash.
#include <core/base/size_table.h>
#include <domain/ruins/assembler.h>

using namespace engine;

// Position (12), member and building (8), wall, slot and height (6), kind and yaw (2): no padding.
ENGINE_EXPECT_SIZE(28, 4, ruins::Instance);
// Three 2-vectors (24), height and reach (8), building (4), wall, windward and a spare byte (4).
ENGINE_EXPECT_SIZE(40, 4, ruins::Drift);
// Seed (8), tile (8), origin (12), four counts (16), walls, shape and yaw (4).
ENGINE_EXPECT_SIZE(48, 8, ruins::Site);
