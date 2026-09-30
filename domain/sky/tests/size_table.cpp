// Size table for domain/sky (ADR-0019). Nothing here is hot: one ephemeris a frame and one star
// table a scene. The star is the one record there are thousands of — about 8,500 a sky — and the
// state is what crosses the registry once a frame.
#include <core/base/size_table.h>
#include <domain/scene_gen/sky.h>
#include <domain/sky/ephemeris.h>

using namespace engine;

// Direction (12), magnitude (4), colour (12).
ENGINE_EXPECT_SIZE(28, 4, scene_gen::Star);
// Five directions, and the calendar and the distances in doubles.
ENGINE_EXPECT_SIZE(24, 8, sky::Direction);
ENGINE_EXPECT_SIZE(24, 8, sky::Calendar);
