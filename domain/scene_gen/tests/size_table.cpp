// Size table for domain/scene_gen (ADR-0019). A placement is the one record a generator makes per
// instance — a thousand ruins are some fifty thousand of them, made per tile a world activates and
// copied once into the scene's instances — so its stride is what a streamed tile's output costs.
#include <core/base/size_table.h>
#include <domain/scene_gen/scene_gen.h>

using namespace engine;

// Mesh (4), the transform — position, rotation, scale — (40), the tag (1) and three bytes of the
// struct's own alignment.
ENGINE_EXPECT_SIZE(48, 4, scene_gen::Placement);
ENGINE_EXPECT_SIZE(8, 4, scene_gen::TileCoord);
// A ring's place, millimetres: centre and half-side.
ENGINE_EXPECT_SIZE(24, 8, scene_gen::RingPlace);
