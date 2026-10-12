// Size table for systems/scatter (ADR-0019): a placement as the layer decides it, which a whole
// read holds a hundred thousand of before it becomes the scene's instances.
#include <core/base/size_table.h>
#include <systems/scatter/scatter.h>

using namespace engine;

// Two i64 millimetres, an f64 height, a float32 quaternion, yaw, scale, kind and rank.
ENGINE_EXPECT_SIZE(56, 8, scatter::Point);
