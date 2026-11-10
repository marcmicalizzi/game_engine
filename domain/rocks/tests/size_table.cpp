// Size table for domain/rocks (ADR-0019). The solver runs at content-build time and keeps no
// per-instance or per-frame state, so it has no hot type; the one record a caller keeps — the
// budget's account of a rock — is pinned so that a field added to it is a decision.
#include <core/base/size_table.h>
#include <domain/rocks/deposits.h>
#include <domain/rocks/rocks.h>

using namespace engine;

ENGINE_EXPECT_SIZE(36, 4, rocks::RockReport);
ENGINE_EXPECT_SIZE(76, 4, rocks::CliffReport);
ENGINE_EXPECT_SIZE(24, 4, rocks::RockSurface);
// Offline recipe controls; no runtime instance grows with this geological history. 60 since W85
// (2026-11-05): `from_formation`, one bool and its padding, which says whether a recipe that
// carries the scene's formation takes its beds from it.
ENGINE_EXPECT_SIZE(60, 4, rocks::RockGeology);

// 144 since G143 (2026-11-10): the plan's cones, blocks and per-sample rock, three vectors; a plan
// is built once an entry, on open, and kept for the entry's life.
ENGINE_EXPECT_SIZE(144, 8, rocks::DepositPlan);
// G143's records, one a cone and one a block of a plan: offline, never per frame or per instance.
ENGINE_EXPECT_SIZE(56, 8, rocks::DepositCone);
ENGINE_EXPECT_SIZE(128, 8, rocks::DepositPlanBlock);
