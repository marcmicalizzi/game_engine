// Size table for systems/scene_collision (ADR-0019). Nothing here is per piece or per sample: a
// tile's pieces are the backend's compound and its heights plain floats. What a host hands the
// consumer every tick is the drawn ground's time, so that is what is pinned.
#include <core/base/size_table.h>
#include <systems/scene_collision/scene_collision.h>

using namespace engine;

ENGINE_EXPECT_SIZE(32, 8, scene_collision::GroundTime);
