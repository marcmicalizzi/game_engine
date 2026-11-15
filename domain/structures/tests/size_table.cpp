// Size table for domain/structures (ADR-0019; AGENTS.md, "Size table"). The walk records are
// kept an element of a building's floors, walls, openings and flights and walked by whoever
// builds collision or a walking test from them.
#include <core/base/size_table.h>
#include <domain/structures/structures.h>

using namespace engine;

ENGINE_EXPECT_SIZE(16, 4, structures::WalkFloor);
ENGINE_EXPECT_SIZE(24, 4, structures::WalkWell);
ENGINE_EXPECT_SIZE(32, 4, structures::WalkWall);
ENGINE_EXPECT_SIZE(48, 4, structures::WalkOpening);
ENGINE_EXPECT_SIZE(44, 4, structures::WalkFlight);
ENGINE_EXPECT_SIZE(28, 4, structures::WalkBox);
ENGINE_EXPECT_SIZE(16, 4, structures::RoomLink);
