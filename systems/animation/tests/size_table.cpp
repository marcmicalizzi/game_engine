// Size table for systems/animation (ADR-0019). TODO(animation): every hot type this capability adds —
// components, per-instance solver state, GPU-mirrored structs — gets an entry, so that footprint
// regressions fail the build rather than the frame rate.
#include <core/base/size_table.h>

#include <systems/animation/animation.h>

using namespace engine;

ENGINE_EXPECT_SIZE(16, 8, animation::AnimationSystem);
