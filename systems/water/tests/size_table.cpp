// Size table for systems/water (ADR-0019). The capability adds no hot type of its own: a body and
// its mesh are read once per scene, and the per-pixel numbers are a `gfx::ResolveMaterial`, which
// gfx's table pins. The one type the renderer copies per water mesh is the shading block.
#include <core/base/size_table.h>
#include <domain/gfx/visibility_resolve.h>

using namespace engine;

ENGINE_EXPECT_SIZE(60, 4, gfx::WaterShading);
