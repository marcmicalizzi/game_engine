// Size table for domain/routes (ADR-0019). The capability has no hot type: the plan is decided once
// when a scene is read and the tread is the ground's. The one record kept per station of every line
// is pinned here, since it is written to the plan's file as its bytes and a region's lines hold
// tens of thousands of them.
#include <core/base/size_table.h>
#include <domain/routes/routes.h>

using namespace engine;

ENGINE_EXPECT_SIZE(32, 8, routes::Station);
