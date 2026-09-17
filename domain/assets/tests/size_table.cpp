// Size table for domain/assets (ADR-0019). Primitive is one entry per draw range of an imported
// mesh, so it is pinned exactly. Material holds a std::string, whose size moves with the
// standard library's debug settings, so its entry pins everything around the name instead.
#include <core/base/size_table.h>
#include <domain/assets/gltf.h>

#include <string>

using namespace engine;

ENGINE_EXPECT_SIZE(12, 4, assets::Primitive);

ENGINE_EXPECT_SIZE(sizeof(std::string) + 72, 8, assets::Material);
