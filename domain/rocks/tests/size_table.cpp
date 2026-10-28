// Size table for domain/rocks (ADR-0019). The solver runs at content-build time and keeps no
// per-instance or per-frame state, so it has no hot type; the one record a caller keeps — the
// budget's account of a rock — is pinned so that a field added to it is a decision.
#include <core/base/size_table.h>
#include <domain/rocks/rocks.h>

using namespace engine;

ENGINE_EXPECT_SIZE(36, 4, rocks::RockReport);
ENGINE_EXPECT_SIZE(76, 4, rocks::CliffReport);
ENGINE_EXPECT_SIZE(24, 4, rocks::RockSurface);
// Offline recipe controls; no runtime instance grows with this geological history.
ENGINE_EXPECT_SIZE(56, 4, rocks::RockGeology);
