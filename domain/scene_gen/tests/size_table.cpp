// Size table for domain/scene_gen (ADR-0019). A placement is the one record a generator makes per
// instance — a thousand ruins are some fifty thousand of them, made per tile a world activates and
// copied once into the scene's instances — so its stride is what a streamed tile's output costs.
#include <core/base/size_table.h>
#include <domain/scene_gen/scene_gen.h>

using namespace engine;

// The position, a `WorldPos` (24, ADR-0053), the rotation (16), the scale (12), the mesh (4), the
// tag (1) and seven bytes of the struct's own alignment, which the f64s make 8. It was 48 bytes,
// 4-aligned, while the position was a float32 `Transform3`'s (until 2026-10-06): 16 bytes more an
// instance a generator makes, which a tile copies once into the scene's instances.
ENGINE_EXPECT_SIZE(64, 8, scene_gen::Placement);
ENGINE_EXPECT_SIZE(8, 4, scene_gen::TileCoord);
// A ring's place, millimetres: centre and half-side.
ENGINE_EXPECT_SIZE(24, 8, scene_gen::RingPlace);
