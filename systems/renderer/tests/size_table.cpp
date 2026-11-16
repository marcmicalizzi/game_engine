// Size table for systems/renderer (ADR-0019): the types the renderer mirrors on the GPU itself
// rather than through `gfx` — the pair expansion's and the opaque shadow proxy's bake's push
// constants (gpu_scene.h; pair_expand.slang, alpha_coverage.slang) and the overlay's vertex
// (overlay.h; overlay.slang's five words). A change to one is a change to its shader in the same
// commit.
#include <core/base/size_table.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/overlay.h>

using namespace engine;

ENGINE_EXPECT_SIZE(40, 8, renderer::PairExpandParams);
ENGINE_EXPECT_SIZE(64, 8, renderer::AlphaCoverageParams);
ENGINE_EXPECT_SIZE(20, 4, renderer::OverlayVertex);
