// Size table for domain/ecs (ADR-0019).
//
// flecs owns the component storage, so the hot types here are flecs' and not the engine's; what
// this module contributes is configuration and the phase table, both of which are per world and
// cold. `flecs::entity` is pinned anyway because `Phases` is an array of eight of them and the
// engine would want to know if that ever stopped being a world pointer plus an id.
#include <core/base/size_table.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/systems.h>

using namespace engine;
using namespace engine::ecs;

ENGINE_EXPECT_SIZE(16, 8, flecs::entity);
ENGINE_EXPECT_SIZE(128, 8, Phases);
ENGINE_EXPECT_SIZE(40, 8, SimWorldConfig);
ENGINE_EXPECT_SIZE(1, 1, TickPhase);
ENGINE_EXPECT_SIZE(12, 4, TableWatchConfig);

// The seam types (ADR-0028). `ComponentType` is one row of a per-world table walked whenever a
// mask is built or a command is applied, and `Identity` is on every entity a save file contains,
// so a byte here is a byte per persistent entity.
ENGINE_EXPECT_SIZE(16, 8, Identity);
ENGINE_EXPECT_SIZE(32, 8, ComponentType);
ENGINE_EXPECT_SIZE(1, 1, Transient);
