// Size table for domain/village (ADR-0019): the plan's records, kept one a plot and one a station
// while an entry is open and walked whenever a tile asks what stands on it.
#include <core/base/size_table.h>
#include <domain/village/village.h>

using namespace engine;

ENGINE_EXPECT_SIZE(48, 8, village::LaneStation);
// 288: G207 keeps the setback each plot's house was asked for (`setback_mm`), so the plan says it
// and no consumer re-derives the draw.
ENGINE_EXPECT_SIZE(288, 8, village::Plot);
