// Size table for core/ids (ADR-0019).
#include <core/base/size_table.h>
#include <core/ids/id128.h>

using namespace engine;

ENGINE_EXPECT_SIZE(16, 8, Id128);
ENGINE_EXPECT_SIZE(16, 8, IdGenerator);
