// Size table for foundation/image (ADR-0019). A decoded Image is one header plus a Vector, so
// it is cheap to return by value and to keep in a cache of loaded textures.
#include <core/base/size_table.h>
#include <foundation/image/decode.h>

using namespace engine;

ENGINE_EXPECT_SIZE(12, 4, image::ImageInfo);

// 3 u32 plus padding plus Vector<u8> (16 bytes: pointer and two u32 counts).
ENGINE_EXPECT_SIZE(32, 8, image::Image);
