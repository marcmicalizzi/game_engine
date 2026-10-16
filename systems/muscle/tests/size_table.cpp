// Size table for systems/muscle (ADR-0019): the hot row a tick reads per actuator, and the
// committed record a save writes. A change to either is a change to what a tick touches or what a
// save holds, and says why in its commit.
#include <core/base/size_table.h>
#include <systems/muscle/law.h>
#include <systems/muscle/scheme.h>
#include <systems/muscle/store.h>

#include <schemas/muscle.h>

using namespace engine;

// 28 doubles: the law's derived constants, read once per actuator per tick.
ENGINE_EXPECT_SIZE(224, 8, muscle::LawRow);
// One probe's record, on the stack: three stage states, fifteen work terms, the cache.
ENGINE_EXPECT_SIZE(240, 8, muscle::Probe);
// The committed record (schemas/muscle.schema): identities, mechanics, cache, supervisory fields.
ENGINE_EXPECT_SIZE(216, 8, muscle::LineState);
// The supervisor's half of the record in the store, read and written by every supervised tick.
ENGINE_EXPECT_SIZE(40, 8, muscle::Supervisory);
// The supervisor's per-actuator slots a tick writes: one TickWork, four ValidationEvents.
ENGINE_EXPECT_SIZE(152, 8, muscle::TickWork);
ENGINE_EXPECT_SIZE(160, 8, muscle::ValidationEvent);
