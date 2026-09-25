// Size table for domain/assets (ADR-0019). Primitive is one entry per draw range of an imported
// mesh, so it is pinned exactly. Material holds a std::string, whose size moves with the
// standard library's debug settings, so its entry pins everything around the name instead.
#include <core/base/size_table.h>
#include <domain/assets/gltf.h>

#include <string>

using namespace engine;

// 16, not 12: a primitive now records which skin its node had, which is also what says whether
// its vertices were left in bind space or flattened to world space.
ENGINE_EXPECT_SIZE(16, 4, assets::Primitive);

// 216 around the name, not 72: the occlusion strength and how each of the five slots samples its
// image (a 5-byte sampler and a 20-byte transform, 28 bytes a slot with alignment) came with the
// samplers and KHR_texture_transform. Import-time data, never per frame, so the growth is not a
// hot path's.
ENGINE_EXPECT_SIZE(sizeof(std::string) + 216, 8, assets::Material);
