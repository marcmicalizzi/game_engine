// Size table for systems/npc (ADR-0019). A resident's footprint is 05 §5.6's budget line — "~200 B
// at LOD3" — so every type held once per resident is pinned here, and a byte on any of them is
// 100 KB at the Phase 3 exit's population (docs/subsystems/npc.md, "Storage per resident").
#include <core/base/size_table.h>
#include <systems/npc/npc.h>
#include <systems/npc/routine.h>

using namespace engine;

// The components on a resident's entity. NpcState is the written-back part, widest field first; its
// anchor is a worldpos (ADR-0053), so 40 bytes where a float32 anchor made it 24. NpcRoutine is the
// clock offset, four place ids and a routine byte,
// padded to 80.
ENGINE_EXPECT_SIZE(40, 8, npc::NpcState);
ENGINE_EXPECT_SIZE(80, 8, npc::NpcRoutine);
ENGINE_EXPECT_SIZE(12, 4, npc::NpcPlace);
// The capability's per-resident arrays: the drawn variation (two day plans of twelve minute
// offsets, the days-off mask and the routine) and the current point.
ENGINE_EXPECT_SIZE(28, 2, npc::DayPlan);
ENGINE_EXPECT_SIZE(60, 2, npc::Variation);
ENGINE_EXPECT_SIZE(32, 8, npc::RoutinePoint);
ENGINE_EXPECT_SIZE(10, 2, npc::RoutineRow);
// The schedule index's entry, once per resident of the document whether it is held or not: id,
// clock offset, the tiles of its four places, its watch timer, routine, the mask of roles with a
// place, and two flags (docs/subsystems/npc.md, "The schedule index").
ENGINE_EXPECT_SIZE(72, 8, npc::ScheduledResident);
// Everything the capability holds for one resident outside flecs and the wheel.
static_assert(npc::NpcSystem::bytes_per_resident() == 201,
              "size table: a resident's arrays changed; update the table and npc.md");
