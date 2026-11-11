// Size table for domain/walls (ADR-0019): what an open entry keeps resident a module (a slot and
// its tile reference), a piece of the far form, and a solved stone of a report.
#include <core/base/size_table.h>
#include <domain/walls/walls.h>

using namespace engine;

ENGINE_EXPECT_SIZE(64, 8, walls::Slot);
ENGINE_EXPECT_SIZE(40, 8, walls::Piece);
ENGINE_EXPECT_SIZE(16, 4, walls::TileRef);
ENGINE_EXPECT_SIZE(28, 4, walls::Stone);
