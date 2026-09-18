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

ENGINE_EXPECT_SIZE(sizeof(std::string) + 72, 8, assets::Material);
