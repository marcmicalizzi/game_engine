#include <core/base/size_table.h>
#include <domain/deadwood/deadwood.h>

// No hot/runtime types added. Pin the cold report transferred to content consumers.
ENGINE_EXPECT_SIZE(40, 4, engine::deadwood::Report);
