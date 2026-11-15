// Size table for domain/dwellings (ADR-0019): the records of an interior plan. None is walked by
// a frame; they are pinned because a description carries them and a house's plan should stay a
// few hundred bytes.
#include <core/base/size_table.h>
#include <domain/dwellings/dwellings.h>

using namespace engine;

ENGINE_EXPECT_SIZE(20, 4, dwellings::Room);
ENGINE_EXPECT_SIZE(28, 4, dwellings::InnerWall);
ENGINE_EXPECT_SIZE(12, 4, dwellings::Doorway);
ENGINE_EXPECT_SIZE(16, 4, dwellings::OuterOpening);
ENGINE_EXPECT_SIZE(28, 4, dwellings::Hearth);
ENGINE_EXPECT_SIZE(80, 4, dwellings::Stair);
ENGINE_EXPECT_SIZE(32, 4, dwellings::Member);
ENGINE_EXPECT_SIZE(36, 4, dwellings::Piece);
