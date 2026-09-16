// Size table for core/math (ADR-0019). GPU-facing layouts: no padding beyond the natural one.
#include <core/base/size_table.h>
#include <core/math/math.h>

#include <type_traits>

using namespace engine;

ENGINE_EXPECT_SIZE(8, 4, Vec2);
ENGINE_EXPECT_SIZE(12, 4, Vec3);
ENGINE_EXPECT_SIZE(16, 4, Vec4);
ENGINE_EXPECT_SIZE(8, 4, Vec2i);
ENGINE_EXPECT_SIZE(12, 4, Vec3i);
ENGINE_EXPECT_SIZE(16, 4, Quat);
ENGINE_EXPECT_SIZE(36, 4, Mat3);
ENGINE_EXPECT_SIZE(64, 4, Mat4);
ENGINE_EXPECT_SIZE(40, 4, Transform3);
ENGINE_EXPECT_SIZE(24, 4, Aabb3);

static_assert(std::is_trivially_copyable_v<Vec3> && std::is_trivially_copyable_v<Mat4> &&
              std::is_trivially_copyable_v<Transform3>);
