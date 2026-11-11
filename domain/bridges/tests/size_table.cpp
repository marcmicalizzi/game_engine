// Size table for domain/bridges (ADR-0019; AGENTS.md, "Size table"). A bridge's plan keeps one
// `Member` an element (a stone, a slab, a plank), walked to emit its mesh and held by the tests.
#include <core/base/size_table.h>
#include <domain/bridges/bridges.h>

using namespace engine;

ENGINE_EXPECT_SIZE(40, 4, bridges::Member);
