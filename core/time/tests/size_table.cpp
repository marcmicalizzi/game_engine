// Size table for core/time (ADR-0019).
#include <core/base/size_table.h>
#include <core/time/time.h>

using namespace engine;

ENGINE_EXPECT_SIZE(8, 8, SimTick);
ENGINE_EXPECT_SIZE(8, 8, GameTime);
ENGINE_EXPECT_SIZE(8, 8, time::Stopwatch);
ENGINE_EXPECT_SIZE(48, 8, FixedStepClock);
ENGINE_EXPECT_SIZE(24, 8, GameClock);
