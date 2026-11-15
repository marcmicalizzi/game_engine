// Size table for domain/village (ADR-0019): the plan's records, kept one a plot and one a station
// while an entry is open and walked whenever a tile asks what stands on it.
#include <core/base/size_table.h>
#include <domain/village/village.h>

using namespace engine;

ENGINE_EXPECT_SIZE(24, 8, village::LaneStation);
ENGINE_EXPECT_SIZE(224, 8, village::Plot);
