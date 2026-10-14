// Size table for systems/muscle (ADR-0019): the hot row a tick reads per actuator, and the
// committed record a save writes. A change to either is a change to what a tick touches or what a
// save holds, and says why in its commit.
#include <core/base/size_table.h>
#include <systems/muscle/law.h>
#include <systems/muscle/scheme.h>

#include <schemas/muscle.h>

using namespace engine;

// 28 doubles: the law's derived constants, read once per actuator per tick.
ENGINE_EXPECT_SIZE(224, 8, muscle::LawRow);
// One probe's record, on the stack: three stage states, fifteen work terms, the cache.
ENGINE_EXPECT_SIZE(240, 8, muscle::Probe);
// The committed record (schemas/muscle.schema): identities, mechanics, cache, supervisory fields.
ENGINE_EXPECT_SIZE(216, 8, muscle::LineState);
