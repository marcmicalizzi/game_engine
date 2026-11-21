// Size table for systems/renderer (ADR-0019): the types the renderer mirrors on the GPU itself
// rather than through `gfx` — the pair expansion's and the opaque shadow proxy's bake's push
// constants (gpu_scene.h; pair_expand.slang, alpha_coverage.slang) and the overlay's vertex
// (overlay.h; overlay.slang's five words), the eye adaptation's state and parameters (exposure.h,
// exposure_pass.h; exposure.slang) and the accumulation's parameters (accumulation_pass.h;
// accumulate.slang; review A8, batch 68). A change to one is a change to its shader in the same
// commit.
#include "../src/accumulation_pass.h"
#include "../src/exposure_pass.h"

#include <core/base/size_table.h>
#include <systems/renderer/exposure.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/overlay.h>

using namespace engine;

ENGINE_EXPECT_SIZE(40, 8, renderer::PairExpandParams);
ENGINE_EXPECT_SIZE(64, 8, renderer::AlphaCoverageParams);
ENGINE_EXPECT_SIZE(20, 4, renderer::OverlayVertex);
ENGINE_EXPECT_SIZE(32, 4, renderer::ExposureState);
ENGINE_EXPECT_SIZE(96, 8, renderer::ExposureParams);
ENGINE_EXPECT_SIZE(152, 8, renderer::AccumulateParams);
