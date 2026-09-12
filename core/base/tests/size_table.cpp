// Size table for core/base. base has no hot types yet; this file exists so the pattern is
// in place and the macro is exercised by the build.
#include <core/base/size_table.h>
#include <core/base/types.h>

ENGINE_EXPECT_SIZE(4, 4, engine::u32);
ENGINE_EXPECT_SIZE(8, 8, engine::u64);
ENGINE_EXPECT_SIZE(8, 8, engine::usize);
