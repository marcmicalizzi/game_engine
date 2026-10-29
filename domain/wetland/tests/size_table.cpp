#include <core/base/size_table.h>
#include <domain/wetland/wetland.h>
using namespace engine;
ENGINE_EXPECT_SIZE(28, 4, wetland::Profile);
ENGINE_EXPECT_SIZE(24, 4, wetland::Report);
