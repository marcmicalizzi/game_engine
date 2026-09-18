// Size table for domain/ecs (ADR-0019).
//
// flecs owns the component storage, so the hot types here are flecs' and not the engine's; what
// this module contributes is configuration and the phase table, both of which are per world and
// cold. `flecs::entity` is pinned anyway because `Phases` is an array of eight of them and the
// engine would want to know if that ever stopped being a world pointer plus an id.
#include <core/base/size_table.h>
#include <domain/ecs/sim_world.h>

using namespace engine;
using namespace engine::ecs;

ENGINE_EXPECT_SIZE(16, 8, flecs::entity);
ENGINE_EXPECT_SIZE(128, 8, Phases);
ENGINE_EXPECT_SIZE(24, 8, SimWorldConfig);
ENGINE_EXPECT_SIZE(1, 1, TickPhase);
