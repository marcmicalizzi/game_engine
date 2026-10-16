// Size table for systems/muscle_sim (ADR-0019): what the tick reads and writes per actuator besides
// the store's own rows (systems/muscle/tests/size_table.cpp).
#include <core/base/size_table.h>
#include <systems/muscle_sim/path.h>

#include <schemas/muscle.h>

using namespace engine;

// One point of a route: a joint index and an offset, read per actuator per tick.
ENGINE_EXPECT_SIZE(16, 4, muscle_sim::RoutePoint);
// The path's history at the last boundary, read and written per actuator per tick.
ENGINE_EXPECT_SIZE(24, 8, muscle_sim::PathHistory);
// The tick's output component, written per actuator per tick.
ENGINE_EXPECT_SIZE(32, 8, muscle::LineOutput);
